#!/usr/bin/env node
'use strict';

// Reconstructs the Dart source tree embedded in the official `kernel_blob.bin`.
//
// The official kernel keeps uriToSource for every library compiled into the
// application (package:skyline, package:flutter and its dependencies).  This
// tool writes those sources back into a package layout and emits a
// `package_config.json`, so the application can be recompiled against the public
// Dart 3.9.2 SDK plus the Skyline `dart:ui` compatibility layer.
//
// Only `package:` sources are written.  `dart:`/`org-dartlang-sdk:` sources are
// provided by the SDK, and the generated `dart_plugin_registrant.dart` is
// replaced by the host (it only registers platform plugins on mobile/desktop).
//
// Usage:
//   node tools/reconstruct-kernel-sources.js --out DIR
//   node tools/reconstruct-kernel-sources.js --out DIR --check
//
// Discovery can be overridden with SKYLINE_KERNEL_BLOB (same as
// tools/extract-kernel-symbols.js).

const fs = require('node:fs');
const path = require('node:path');

const { parseKernel, discoverInputs } = require('./extract-kernel-symbols.js');

const repoRoot = path.resolve(__dirname, '..');
const defaultOut = path.join(repoRoot, 'native', 'engine', 'work', 'skyline-app');
const defaultLanguageVersion = '3.9';

// Packages whose pubspec language version predates Dart 3.0.  Libraries with a
// pre-3.0 language version may still extend pre-`interface class` types such as
// StreamSubscription (rxdart does), and the official build compiled them that
// way.
const languageVersions = {
  rxdart: '2.19',
};

function usage() {
  return (
    'usage: node tools/reconstruct-kernel-sources.js --out DIR [--check]\n' +
    '  --out DIR  directory that receives packages/ and package_config.json\n' +
    '  --check    report what would be written; do not modify the filesystem\n'
  );
}

function parseArgs(argv) {
  const opts = { out: defaultOut, check: false };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--out') {
      if (i + 1 >= argv.length) throw new Error('--out needs a value');
      opts.out = path.resolve(argv[++i]);
    } else if (arg.startsWith('--out=')) {
      opts.out = path.resolve(arg.slice('--out='.length));
    } else if (arg === '--check') {
      opts.check = true;
    } else if (arg === '-h' || arg === '--help') {
      opts.help = true;
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  return opts;
}

function packageSourcePath(uri) {
  // package:<name>/<path>
  const match = /^package:([A-Za-z_][A-Za-z0-9_]*)\/(.*)$/.exec(uri);
  if (match === null) return null;
  const [, name, rest] = match;
  if (rest.length === 0) return { name, relative: '' };
  // Reject traversal just in case; package URIs in a kernel are compiler output.
  const normalized = path.posix.normalize(rest);
  if (normalized.startsWith('..') || path.posix.isAbsolute(normalized)) {
    throw new Error(`unsafe package path in kernel source URI: ${uri}`);
  }
  return { name, relative: normalized };
}

function collectPackages(sources) {
  const packages = new Map();
  const duplicates = [];
  for (const source of sources) {
    const parsed = packageSourcePath(source.importUri);
    if (parsed === null) continue;
    if (!packages.has(parsed.name)) packages.set(parsed.name, new Map());
    const files = packages.get(parsed.name);
    if (files.has(parsed.relative)) {
      duplicates.push(source.importUri);
      continue;
    }
    files.set(parsed.relative, source.source);
  }
  return { packages, duplicates };
}

function writeSources(outDir, packages) {
  const written = [];
  for (const [name, files] of packages) {
    for (const [relative, source] of files) {
      const dest = path.join(outDir, 'packages', name, 'lib', relative);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.writeFileSync(dest, source);
      written.push(dest);
    }
  }
  return written;
}

function renderPackageConfig(packages) {
  const entries = [...packages.keys()].sort().map(name => ({
    name,
    rootUri: `packages/${name}`,
    packageUri: 'lib/',
    languageVersion: languageVersions[name] || defaultLanguageVersion,
  }));
  return {
    configVersion: 2,
    packages: entries,
  };
}

function verify(outDir, packages) {
  const missing = [];
  for (const [name, files] of packages) {
    for (const [relative] of files) {
      const dest = path.join(outDir, 'packages', name, 'lib', relative);
      if (!fs.existsSync(dest)) missing.push(`${name}/${relative}`);
    }
  }
  return missing;
}

function main() {
  let opts;
  try {
    opts = parseArgs(process.argv.slice(2));
  } catch (error) {
    console.error(error.message);
    console.error(usage());
    return 2;
  }
  if (opts.help) {
    process.stdout.write(usage());
    return 0;
  }

  const inputs = discoverInputs();
  if (!inputs.kernel || !fs.existsSync(inputs.kernel)) {
    console.error('official kernel_blob.bin not found; set SKYLINE_KERNEL_BLOB or SKYLINE_WIN_RESOURCES');
    return 1;
  }

  const warnings = [];
  const kernel = parseKernel(fs.readFileSync(inputs.kernel), warnings);
  const { packages, duplicates } = collectPackages(kernel.sources);
  const fileCount = [...packages.values()].reduce((sum, files) => sum + files.size, 0);

  console.log(
    `kernel: ${kernel.sources.length} sources, ${packages.size} packages, ${fileCount} package files`,
  );
  if (duplicates.length > 0) {
    console.warn(`duplicate package sources ignored: ${duplicates.length}`);
  }
  for (const warning of warnings) console.warn(`warning: ${warning}`);

  const config = renderPackageConfig(packages);
  const configPath = path.join(opts.out, 'package_config.json');

  if (opts.check) {
    const missing = verify(opts.out, packages);
    if (missing.length > 0) {
      console.error(`${missing.length} reconstructed sources missing under ${opts.out}`);
      console.error(missing.slice(0, 10).join('\n'));
      return 1;
    }
    if (!fs.existsSync(configPath)) {
      console.error(`missing ${configPath}; rerun without --check`);
      return 1;
    }
    const committed = JSON.parse(fs.readFileSync(configPath, 'utf8'));
    if (JSON.stringify(committed) !== JSON.stringify(config)) {
      console.error(`${configPath} is out of date; rerun without --check`);
      return 1;
    }
    console.log(`reconstruction is up to date: ${opts.out}`);
    return 0;
  }

  fs.rmSync(path.join(opts.out, 'packages'), { recursive: true, force: true });
  const written = writeSources(opts.out, packages);
  fs.mkdirSync(opts.out, { recursive: true });
  fs.writeFileSync(configPath, JSON.stringify(config, null, 2) + '\n');

  const root = packages.get('sample');
  const rootLibrary = root && root.has('devtools.dart') ? 'packages/sample/lib/devtools.dart' : null;

  console.log(`wrote ${written.length} sources to ${path.join(opts.out, 'packages')}`);
  console.log(`wrote ${configPath}`);
  if (rootLibrary) {
    console.log(`root library: ${path.join(opts.out, rootLibrary)}`);
  } else {
    console.warn('package:sample/devtools.dart not found; choose a root library manually');
  }
  return 0;
}

if (require.main === module) {
  process.exitCode = main();
}

module.exports = {
  packageSourcePath,
  collectPackages,
  renderPackageConfig,
};

'use strict';

// Phase-3 appservice smoke test: boot the recompiled `package:skyline` kernel
// against the stub Linux `libskyline.so` runtime and capture the first Flutter
// frame.  This exercises the real `initSkylineForDevtools` path:
//
//   devtools.dart main -> SkylineApplicationDevtools -> openSkylineDylib
//   -> InitDartAPIDLSkyline -> 73 symbol lookups -> setupFFIFunctions(Register)
//   -> ShareRuntimeFromRuntimeShell / CreateWindow -> runApp -> first frame
//
// Skips when the engine, host, stub runtime or recompiled kernel is missing.
// Set SKYLINE_REQUIRE_ENGINE=1 to fail instead of skipping in CI.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { devtoolsEntrypointArgs } = require('../../test/lib/host');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build', 'skyline_host');
const runtime = path.join(repoRoot, 'native', 'build', 'libskyline.so');
const assets = path.join(
  repoRoot, 'native', 'engine', 'work', 'skyline-app', 'build', 'flutter_assets',
);
const kernel = path.join(assets, 'kernel_blob.bin');
const engineDir = path.join(repoRoot, 'native', 'engine');
const engineIcu = path.join(engineDir, 'out', 'linux-x64-debug', 'icudtl.dat');
const toolchainIcu = path.join(engineDir, 'toolchain', 'engine-artifacts', 'icudtl.dat');
const icu = fs.existsSync(engineIcu) ? engineIcu : toolchainIcu;
const engineLib = path.join(engineDir, 'out', 'linux-x64-debug', 'libflutter_engine.so');

const width = 320;
const height = 240;
const frameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host (npm run native:build)');
  if (!fs.existsSync(runtime)) missing.push('native/build/libskyline.so (npm run native:build)');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so (npm run engine:build)');
  if (!fs.existsSync(kernel)) missing.push('recompiled kernel (npm run kernel:skyline)');
  if (!fs.existsSync(icu)) missing.push('icudtl.dat');
  return missing.length ? missing.join('; ') : null;
}

function parseResult(stdout) {
  const match = /^SKYLINE_HOST_RESULT (.*)$/m.exec(stdout || '');
  if (!match) return null;
  try {
    return JSON.parse(match[1]);
  } catch {
    return null;
  }
}

test('appservice: stub runtime exports all 73 addon lookup symbols', {
  skip: fs.existsSync(runtime) ? false : 'native/build/libskyline.so (npm run native:build)',
}, () => {
  const spec = JSON.parse(
    fs.readFileSync(path.join(repoRoot, 'spec', 'kernel-symbols.json'), 'utf8'),
  );
  const want = new Set(spec.ffi.lookupSymbols.map(entry => entry.name));
  const nm = spawnSync('nm', ['-D', '--defined-only', runtime], { encoding: 'utf8' });
  assert.equal(nm.status, 0, nm.stderr);
  const have = new Set(
    nm.stdout
      .split('\n')
      .map(line => line.trim())
      .filter(Boolean)
      .map(line => line.split(/\s+/).pop()),
  );
  const missing = [...want].filter(name => !have.has(name)).sort();
  assert.deepEqual(missing, [], `missing exports: ${missing.join(', ')}`);
});

test('appservice: recompiled kernel boots against the stub skyline runtime', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_appservice_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [path.dirname(runtime), process.env.LD_LIBRARY_PATH]
      .filter(Boolean)
      .join(':'),
  };

  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '20000',
      '--no-shutdown',
      // package:sample/devtools.dart main(List<String> args)
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: runtime }),
    ], { encoding: 'utf8', timeout: 45000, env });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');

    if (diagnostics.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${diagnostics}`);
      }
      return;
    }

    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.equal(details.width, width);
    assert.equal(details.height, height);
    assert.equal(details.frameSize, frameSize);
    assert.ok(details.vsyncCount >= 1, 'the app never requested a vsync');
    assert.ok(details.presentCount >= 1, 'no frame was presented');

    // The stub runtime was actually dlopen'ed and initialized.
    assert.match(result.stderr, /\[skyline-runtime\] Dart_InitializeApiDL -> 0/);
    assert.match(result.stderr, /\[skyline-runtime:I\]/);
    assert.match(result.stderr, /NotifyFlutterReady -> RunApplication/);
    assert.doesNotMatch(result.stderr, /Failed to load dynamic library/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    // The splash page without a cached image is a white surface.
    assert.deepEqual([...pixels.subarray(0, 4)], [255, 255, 255, 255], 'first pixel must be white');
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

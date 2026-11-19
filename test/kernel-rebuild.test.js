'use strict';

// Phase 3: validates the reconstructed `package:skyline` source tree, the
// `dart:ui` compatibility layer, and (when the local toolchain is present) a
// compile-only smoke of the patched SDK.
//
// The official kernel and the engine checkout are not required for the static
// assertions.  Tests that need them are skipped with a reason.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { parseKernel } = require('../tools/extract-kernel-symbols.js');
const {
  packageSourcePath,
  renderPackageConfig,
} = require('../tools/reconstruct-kernel-sources.js');

const repoRoot = path.resolve(__dirname, '..');
const compatDir = path.join(repoRoot, 'native', 'engine', 'dart_ui_compat');
const workApp = path.join(repoRoot, 'native', 'engine', 'work', 'skyline-app');
const toolchain = path.join(repoRoot, 'native', 'engine', 'toolchain');
const patchedSdk = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-debug', 'patched_sdk');

function readPin(name) {
  const revision = fs.readFileSync(path.join(repoRoot, 'native', 'engine', 'REVISION'), 'utf8');
  const match = new RegExp(`^${name}=(.*)$`, 'm').exec(revision);
  return match ? match[1].trim() : null;
}

test('reconstruction: package URIs map into <pkg>/lib layout', () => {
  assert.deepEqual(packageSourcePath('package:skyline/core/application.dart'), {
    name: 'skyline',
    relative: 'core/application.dart',
  });
  assert.deepEqual(packageSourcePath('package:flutter/src/widgets/binding.dart'), {
    name: 'flutter',
    relative: 'src/widgets/binding.dart',
  });
  assert.equal(packageSourcePath('dart:ui/painting.dart'), null);
  assert.equal(packageSourcePath('file:///tmp/main.dart'), null);
  assert.throws(() => packageSourcePath('package:skyline/../escape.dart'));
});

test('reconstruction: package_config pins the public language version and rxdart 2.19', () => {
  const packages = new Map([
    ['flutter', new Map()],
    ['rxdart', new Map()],
    ['skyline', new Map()],
  ]);
  const config = renderPackageConfig(packages);
  assert.equal(config.configVersion, 2);
  const byName = new Map(config.packages.map(entry => [entry.name, entry]));
  assert.equal(byName.get('skyline').languageVersion, '3.9');
  assert.equal(byName.get('rxdart').languageVersion, '2.19');
  assert.equal(byName.get('skyline').rootUri, 'packages/skyline');
  assert.equal(byName.get('skyline').packageUri, 'lib/');
});

test('dart:ui compat: recovered source surface is present', () => {
  const application = fs.readFileSync(path.join(compatDir, 'application.dart'), 'utf8');
  for (const member of [
    'static Application get current',
    'static Application get top',
    'static Application fromId(',
    'static bool get isAlive',
    'bool isDefaultApplication()',
    'void put(Object key, Object? value)',
    'T? find<T>(Object key)',
    'T get<T extends Object>(Object key, T Function() ifAbsent)',
    'ChannelBuffers get channelBuffers',
    'PlatformDispatcher get platformDispatcher',
    'SingletonFlutterWindow get window',
  ]) {
    assert.ok(application.includes(member), `Application member missing: ${member}`);
  }

  const ext = fs.readFileSync(path.join(compatDir, 'skyline_ext.dart'), 'utf8');
  for (const member of [
    'enum WindowStatus',
    'Paragraph createNativeParagraph()',
    'void warmUpFont(String cacheDir)',
    'typedef SlowRunnerStackCallback',
    'setCheckerboardRasterCacheImages',
    'setCheckerboardPartialPaintAreas',
  ]) {
    assert.ok(ext.includes(member), `compat member missing: ${member}`);
  }

  const mmtrace = fs.readFileSync(path.join(compatDir, 'mmtrace.dart'), 'utf8');
  assert.match(mmtrace, /class MMTrace/);
  assert.match(mmtrace, /class MMFluencyTracker/);
  assert.match(mmtrace, /const pragma entryPoint/);
});

test('dart:ui compat: apply.py patches every required engine file', () => {
  const script = fs.readFileSync(path.join(compatDir, 'apply.py'), 'utf8');
  for (const file of [
    'flutter/lib/ui/ui.dart',
    'flutter/lib/ui/painting.dart',
    'flutter/lib/ui/platform_dispatcher.dart',
    'flutter/lib/ui/window.dart',
    'flutter/lib/ui/text.dart',
    'flutter/third_party/dart/sdk/lib/io/platform.dart',
  ]) {
    assert.ok(script.includes(file), `apply.py does not patch ${file}`);
  }
});

const reconstructedKernel = path.join(workApp, 'build', 'flutter_assets', 'kernel_blob.bin');
const kernelSkip = fs.existsSync(reconstructedKernel)
  ? false
  : 'recompiled kernel missing (run: npm run kernel:skyline)';

test('recompiled kernel uses the public SDK hash and contains package:skyline', {
  skip: kernelSkip,
  timeout: 120000,
}, () => {
  const buf = fs.readFileSync(reconstructedKernel);
  const warnings = [];
  const kernel = parseKernel(buf, warnings);
  const publicHash = readPin('PUBLIC_KERNEL_SDK_HASH');
  assert.ok(kernel.sdkHashes.includes(publicHash), `SDK hashes: ${kernel.sdkHashes.join(', ')}`);
  assert.ok(!kernel.sdkHashes.includes(readPin('OFFICIAL_KERNEL_SDK_HASH')));
  const skyline = kernel.sources.filter(source => source.importUri.startsWith('package:skyline/'));
  assert.equal(skyline.length, 212);
  const root = kernel.sources.find(source => source.importUri === 'package:sample/devtools.dart');
  assert.ok(root, 'root library package:sample/devtools.dart missing');
  assert.match(root.source, /initSkylineForDevtools/);
});

const dartaotruntime = path.join(toolchain, 'dart-sdk', 'bin', 'dartaotruntime');
const frontend = path.join(toolchain, 'engine-artifacts', 'frontend_server_aot.dart.snapshot');
const patchedDill = path.join(patchedSdk, 'platform_strong.dill');
const packageConfig = path.join(workApp, 'package_config.json');
const toolchainSkip =
  fs.existsSync(dartaotruntime) &&
  fs.existsSync(frontend) &&
  fs.existsSync(patchedDill) &&
  fs.existsSync(packageConfig)
    ? false
    : 'toolchain, patched SDK or reconstructed package_config missing ' +
      '(run: npm run toolchain:fetch && npm run engine:build && npm run ui:patch && npm run kernel:skyline)';

test('patched SDK resolves Application / createNativeParagraph / warmUpFont', {
  skip: toolchainSkip,
  timeout: 120000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-ui-compat-'));
  const entry = path.join(dir, 'main.dart');
  fs.writeFileSync(entry, `
import 'dart:ui' as ui;

void main() {
  final ui.Application app = ui.Application.current;
  app.put('k', 42);
  final int? value = app.find<int>('k');
  final String created = app.get<String>('s', () => 's');
  final ui.Paragraph paragraph = ui.createNativeParagraph();
  ui.warmUpFont('/tmp');
  final double refreshRate = ui.PlatformDispatcher.instance.getDisplayRefreshRate();
  final bool parallel = ui.PlatformDispatcher.isUseParallelPipeline();
  final ui.WindowStatus status = ui.WindowStatus.normal;
  final ui.SlowRunnerStackCallback? callback =
      ui.PlatformDispatcher.instance.onReportSlowRunnerStack;
  print('\$value \$created \$paragraph \$refreshRate \$parallel \$status \$callback');
}
`);

  const result = spawnSync(
    dartaotruntime,
    [
      frontend,
      '--sdk-root', `${patchedSdk}/`,
      '--target=flutter',
      '--no-print-incremental-dependencies',
      '--packages', path.join(workApp, 'package_config.json'),
      '--output-dill', path.join(dir, 'out.dill'),
      entry,
    ],
    { encoding: 'utf8', timeout: 90000 },
  );
  fs.rmSync(dir, { recursive: true, force: true });

  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  assert.doesNotMatch(result.stderr, /Error:/);
});

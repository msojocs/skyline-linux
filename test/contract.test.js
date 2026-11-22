'use strict';

// Differential contract tests: replay the same observation cases against the
// Linux implementation and compare them with the recorded official Windows
// module.  Cases are skipped (with a reason) when the Linux implementation or
// the recorded oracle is unavailable.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');

const path = require('node:path');
const { specPath, oraclePaths, linuxSharedMemoryDefault } = require('./lib/env');
const { runOnce } = require('./lib/runner');
const { compareCase } = require('./lib/compare');
const { byId } = require('./cases');

const spec = fs.existsSync(specPath()) ? JSON.parse(fs.readFileSync(specPath(), 'utf8')) : null;
const oracle = oraclePaths();
const linuxShared = linuxSharedMemoryDefault();
// Without an explicit implementation the repository's reference module is the
// target.  It implements the JS-visible contract but has no engine backend.
const referenceAddon = path.resolve(__dirname, '..', 'reference', 'skyline-node.js');
const linuxAddon = process.env.SKYLINE_LINUX_SKYLINE_ADDON || referenceAddon;
const repoRoot = path.resolve(__dirname, '..');
const aotStageBundle = path.resolve(repoRoot, '..', 'wechat-web-devtools-linux',
  'resources', 'app.asar.unpacked', 'node_modules', 'skyline-addon', 'bundle');
const linuxBundle = process.env.SKYLINE_LINUX_BUNDLE || (linuxAddon.includes('build-release') &&
  fs.existsSync(path.join(aotStageBundle, 'libapp.so'))
  ? aotStageBundle
  : [
    path.resolve(repoRoot, 'native', 'engine', 'work', 'skyline-app', 'build'),
    path.resolve(repoRoot, 'apps', 'empty-page', 'build'),
  ].find((candidate) => fs.existsSync(path.join(candidate, 'flutter_assets'))) || null);
// Engine-context cases (create/notify/brightness/destroy) can only run against
// the native addon; the reference implementation intentionally has no engine.
const hasEngineBackend = linuxAddon !== referenceAddon;

const linuxPaths = {
  addon: linuxAddon,
  shared: linuxShared,
  resources: process.env.SKYLINE_LINUX_RESOURCES || linuxBundle,
  bundle: linuxBundle,
};

const comparisonPaths = [oracle, linuxPaths].filter(Boolean);

function linuxSkipFor(caseId) {
  if (!spec) return `oracle spec missing: ${specPath()} (run npm run spec:record)`;
  const entry = spec.cases[caseId];
  if (!entry) return 'case was not recorded from the Windows oracle';
  const definition = byId(caseId);
  if (definition && (definition.engine || definition.engineContext) && !hasEngineBackend) {
    return 'target module has no engine backend';
  }
  if (entry.status === 'skip') return `oracle skipped: ${entry.reason}`;
  if (entry.status === 'crash' || entry.status === 'timeout') {
    return `oracle process did not survive this case (${entry.status}); engine baseline unavailable`;
  }
  const isSharedCase = caseId.startsWith('shm.');
  if (isSharedCase && !linuxShared) return 'Linux sharedMemory module is not available';

  return null;
}

if (spec) {
  const debug = spec.cases['globals.surface'] && spec.cases['globals.surface'].value && spec.cases['globals.surface'].value.debug;
  test('recorded oracle has the expected Skyline metadata', () => {
    assert.ok(debug, 'oracle did not record SkylineDebugInfo');
    assert.equal(debug.skyline_version, '1.4.21');
    assert.match(debug.skyline_git_rev, /^[0-9a-f]{40}$/);
    assert.match(debug.flutter_engine_git_rev, /^[0-9a-f]{40}$/);
  });

  test('recorded oracle pins the SkylineShell method order', () => {
    const entry = spec.cases['class.prototype'];
    assert.ok(entry && entry.value, 'oracle did not record the prototype');
    const expected = [
      'setNotifyBootstrapDoneCallback', 'createWindow', 'destroyWindow',
      'onWindowRasterized', 'setNotifyWindowReadyCallback', 'setNotifyRouteDoneCallback',
      'setNavigateBackCallback', 'setNavigateBackDoneCallback', 'setLoadResourceCallback',
      'setLoadResourceAsyncCallback', 'setHttpRequestCallback', 'setSendLogCallback',
      'setSafeAreaEdgeInsets', 'notifyResourceLoad', 'notifyHttpRequestComplete',
      'dispatchTouchStartEvent', 'dispatchTouchMoveEvent', 'dispatchTouchEndEvent',
      'dispatchTouchOverEvent', 'dispatchTouchCancelEvent', 'dispatchWheelEvent',
      'dispatchCharEvent', 'dispatchKeyboardEvent', 'notifyNavigateTo', 'notifyRedirectTo',
      'notifyNavigateBack', 'notifyAppLaunch', 'notifyReLaunch', 'notifyAutoReLaunch',
      'notifySwitchTab', 'onPlatformBrightnessChanged', 'constructor',
    ];
    assert.deepEqual(entry.value.prototypeKeys, expected);
  });
}

const cases = spec ? Object.keys(spec.cases) : [];

for (const caseId of cases) {
  const skipReason = linuxSkipFor(caseId);
  test(`contract ${caseId}`, { skip: skipReason || false, timeout: 45000 }, async () => {
    const result = await runOnce({
      platform: 'linux',
      caseId,
      paths: linuxPaths,
      timeoutMs: 30000,
      allowEngineContext: hasEngineBackend,
    });
    const comparison = compareCase(caseId, spec.cases[caseId], result, comparisonPaths);
    if (!comparison.ok) {
      assert.fail(`contract mismatch for ${caseId}:\n${comparison.diffs.join('\n')}`);
    }
  });
}

test('contract suite has a recorded oracle', () => {
  if (!spec) {
    assert.fail(`缺少 ${specPath()}；先运行 npm run spec:record`);
  }
  assert.ok(Object.keys(spec.cases).length > 0);
});

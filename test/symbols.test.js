'use strict';

// Guards the committed phase-2 symbol inventory.  These assertions are
// intentionally count-based so a regenerated spec must be reviewed consciously
// when the official bundle or the pinned engine changes.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const repoRoot = path.resolve(__dirname, '..');
const specPath = path.join(repoRoot, 'spec', 'kernel-symbols.json');
const revisionPath = path.join(repoRoot, 'native', 'engine', 'REVISION');

function readRevisions() {
  const pins = {};
  for (const line of fs.readFileSync(revisionPath, 'utf8').split('\n')) {
    const match = /^([A-Z0-9_]+)=(.*)$/.exec(line);
    if (match) pins[match[1]] = match[2];
  }
  return pins;
}

const spec = JSON.parse(fs.readFileSync(specPath, 'utf8'));

test('symbol inventory: pinned engine provenance matches REVISION', () => {
  const pins = readRevisions();
  assert.equal(spec.pins.flutterVersion, pins.FLUTTER_VERSION);
  assert.equal(spec.pins.engineRevision, pins.FLUTTER_GIT_REV);
  assert.equal(spec.pins.dartVersion, pins.DART_VERSION);
  assert.equal(spec.pins.dartGitRev, pins.DART_GIT_REV);
  assert.equal(spec.pins.officialKernelSdkHash, pins.OFFICIAL_KERNEL_SDK_HASH);
  assert.equal(spec.pins.publicKernelSdkHash, pins.PUBLIC_KERNEL_SDK_HASH);
  assert.equal(spec.pins.skylineVersion, '1.4.21');
});

test('symbol inventory: 73 addon FFI lookups are all exported', () => {
  const lookups = spec.ffi.lookupSymbols;
  assert.equal(lookups.length, 73);
  for (const entry of lookups) {
    assert.equal(entry.exported, true, `${entry.name} is not exported by skyline.node`);
  }
  const names = new Set(lookups.map(entry => entry.name));
  for (const required of [
    'InitDartAPIDLSkyline',
    'AllocateFfiTable',
    'CreateWindow',
    'GetFeatureToggle',
    'GetSplashCacheImage',
    'Register',
    'YogaCalculateLayout',
  ]) {
    assert.ok(names.has(required), `missing lookup ${required}`);
  }
});

test('symbol inventory: 375 registered callbacks have stable stringHash values', () => {
  const callbacks = spec.ffi.registeredCallbacks;
  assert.equal(callbacks.length, 375);
  assert.equal(new Set(callbacks.map(entry => entry.name)).size, callbacks.length);
  for (const entry of callbacks) {
    assert.match(entry.name, /^[A-Za-z_][A-Za-z0-9_]*$/);
    assert.ok(Number.isInteger(entry.hash));
    assert.ok(entry.hash >= 0 && entry.hash <= 0x3fffffff, `${entry.name} hash out of smi range`);
  }
  const byName = new Map(callbacks.map(entry => [entry.name, entry.hash]));
  assert.equal(byName.get('DumpElementTree'), 669860362);
  assert.equal(byName.get('SnapshotTakeSnapshot'), 750173314);
  assert.ok(byName.has('DumpElementTree'));
  assert.ok(byName.has('SnapshotTakeSnapshot'));
});

test('symbol inventory: engine @Native symbols cover the dart:ui surface', () => {
  const symbols = new Set(spec.engineNativeSymbols.map(entry => entry.name));
  assert.equal(symbols.size, 275);
  for (const required of [
    'Canvas::drawRect',
    'ParagraphBuilder::build',
    'SceneBuilder::build',
    'PlatformConfigurationNativeApi::ScheduleFrame',
    'PlatformConfigurationNativeApi::Render',
    'flutter_fluency_tracker::DartFluencyStageChange',
    'mm_trace_tools::DartTrace',
  ]) {
    assert.ok(symbols.has(required), `missing engine native symbol ${required}`);
  }
});

test('symbol inventory: private dart:ui additions are recorded', () => {
  const custom = new Map(spec.dartUiCustomMembers.map(entry => [entry.name, entry]));
  const app = custom.get('Application');
  assert.ok(app, 'Application member missing');
  assert.equal(app.kind, 'class');
  for (const member of ['current', 'top', 'fromId', 'get', 'put', 'platformDispatcher', 'window']) {
    assert.ok(app.members.includes(member), `Application.${member} missing`);
  }
  assert.ok(custom.has('createNativeParagraph'));
  assert.ok(custom.has('warmUpFont'));
});

test('symbol inventory: committed file is reproducible', () => {
  const generator = path.join(repoRoot, 'tools', 'extract-kernel-symbols.js');
  const result = spawnSync(process.execPath, [generator, '--check'], {
    encoding: 'utf8',
    timeout: 120000,
  });
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
});

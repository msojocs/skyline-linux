'use strict';

// Phase-2/3 integration: the recompiled `package:skyline` appservice running as
// an AOT snapshot on the release engine with the embedded Skyline runtime.
//
// Prerequisites:
//   npm run runtime:embed -- --runtime-mode release
//   npm run devtools:aot
//   npm run native:build:release
//
// Skips when the AOT/release artifacts are not present (CI restores them from
// the build-deps release and sets SKYLINE_REQUIRE_ENGINE=1).

const test = require('node:test');
const assert = require('node:assert/strict');
const {
  SKYLINE_RELEASE,
  checkBuildCurrent,
  paths,
  pixelAt,
  prerequisiteSkip,
  runHost,
} = require('../../test/lib/host');

const width = 320;
const skip = prerequisiteSkip(SKYLINE_RELEASE);

test('skyline aot: build artifacts match the sources', { skip }, t => {
  checkBuildCurrent(t, ['engine-release', 'aot']);
});

test('skyline aot: appservice stripes render on the release engine', {
  skip,
  timeout: 60000,
}, () => {
  const { result, logs, pixels } = runHost({
    mode: 'release',
    key: 'skyline_aot_ops',
    aotLibrary: paths.skylineAot,
    ops: paths.fixture('appservice-stripes'),
    width,
    height: 240,
    dpr: 1,
    frames: 0,
    timeoutMs: 2500,
    spawnTimeoutMs: 40000,
  });
  assert.equal(result.status, 0, `AOT appservice host failed:\n${logs}`);
  assert.match(logs, /\[host\] AOT snapshot: .*libapp\.so/);
  assert.match(logs, /devtools\.dart main args/);
  assert.match(logs, /appservice ops: 17 ops, 5 nodes -> CreatePageRoute/);
  assert.ok(pixels, 'no frame was captured');
  // Same three stripes as the JIT replay fixture (see native/test/ops).
  assert.deepEqual(pixelAt(pixels, width, 10, 10), [255, 51, 102, 255]);
  assert.deepEqual(pixelAt(pixels, width, 10, 100), [51, 204, 102, 255]);
  assert.deepEqual(pixelAt(pixels, width, 10, 200), [51, 102, 255, 255]);
});

'use strict';

// Phase-2 AOT path: a release Flutter engine launched with `libapp.so`
// (`FlutterEngineCreateAOTData`) instead of a JIT kernel blob.
//
// Prerequisites (documented in native/engine/README.md):
//   npm run engine:build -- --runtime-mode release   # native/engine/out/linux-x64-release
//   npm run app:aot                                  # apps/empty-page/build/libapp.so
//   npm run native:build:release
//
// Skips when the release host/engine/snapshot are not present.

const test = require('node:test');
const assert = require('node:assert/strict');
const { paths, prerequisiteSkip, runHost } = require('../../test/lib/host');

const width = 128;
const height = 96;
const frameSize = width * height * 4;

test('engine aot: release host renders a frame from libapp.so', {
  skip: prerequisiteSkip(['host-release', 'engine-release', 'icu-release', 'empty-page-aot']),
  timeout: 60000,
}, () => {
  const { result, logs, pixels } = runHost({
    mode: 'release',
    key: 'skyline_aot',
    assets: paths.appAssets('empty-page'),
    aotLibrary: paths.appAot('empty-page'),
    width,
    height,
    dpr: 1,
    timeoutMs: 15000,
    entrypoint: false,
    spawnTimeoutMs: 40000,
  });
  assert.equal(result.status, 0, `release host failed:\n${logs}`);
  assert.match(logs, /\[host\] AOT snapshot: .*libapp\.so/);
  assert.match(logs, /SKYLINE_HOST_RESULT/);
  assert.ok(pixels, 'no frame was written to shared memory');
  assert.deepEqual([...pixels.subarray(0, 4)], [255, 0, 0, 255]);
  assert.deepEqual([...pixels.subarray(frameSize - 4)], [255, 0, 0, 255]);
});

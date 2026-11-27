'use strict';

// Phase-2 engine smoke test: boot the Linux Flutter engine with the minimal
// dart:ui page, capture one software-rendered frame, and validate the frame written
// to /dev/shm/skyline_<key>.dat (docs/contract.md section 4).
//
// The test skips when the (multi-GiB) engine build has not been staged.  Set
// SKYLINE_REQUIRE_ENGINE=1 to turn the skip into a failure.

const test = require('node:test');
const assert = require('node:assert/strict');
const { paths, prerequisiteSkip, runHost } = require('../../test/lib/host');

const width = 64;
const height = 48;
const dpr = 2;
const physicalWidth = Math.ceil(width * dpr);
const physicalHeight = Math.ceil(height * dpr);
const expectedFrameSize = physicalWidth * physicalHeight * 4;

test('engine smoke: one software-rendered frame reaches shared memory', {
  skip: prerequisiteSkip(['host-debug', 'engine-debug', 'icu-debug', 'empty-page-kernel']),
  timeout: 60000,
}, () => {
  const { result, logs, details, frame } = runHost({
    key: 'engine_smoke',
    assets: paths.appAssets('empty-page'),
    width,
    height,
    dpr,
    entrypoint: false,
    noShutdown: false,
    spawnTimeoutMs: 30000,
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
  assert.equal(details.width, physicalWidth);
  assert.equal(details.height, physicalHeight);
  assert.equal(details.frameSize, expectedFrameSize);
  assert.ok(details.vsyncCount >= 1, 'the app never requested a vsync');

  assert.ok(frame, 'no frame was written to shared memory');
  assert.equal(frame.version, 1, 'outer header version must be 1');
  assert.equal(frame.padding, 0, 'outer header padding must be zero');
  assert.ok(frame.frameId >= 1, 'frameId must be positive');
  assert.equal(frame.frameSize, expectedFrameSize);
  assert.ok(frame.data.length >= 8 + frame.frameSize, 'data region is too small for the frame');

  // The page is opaque red.  The host converts Skia N32/BGRA to RGBA.
  const pixels = frame.pixels;
  assert.deepEqual([...pixels.subarray(0, 4)], [255, 0, 0, 255], 'first pixel must be opaque red');
  const center = (Math.floor(physicalHeight / 2) * physicalWidth + Math.floor(physicalWidth / 2)) * 4;
  assert.deepEqual([...pixels.subarray(center, center + 4)], [255, 0, 0, 255], 'center pixel must be opaque red');
});

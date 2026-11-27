'use strict';

// Phase-2 viewport resize: FlutterEngineSendWindowMetricsEvent updates the
// implicit view and subsequent frames use the new physical size.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { paths, prerequisiteSkip, runHost } = require('../../test/lib/host');

test('engine resize: viewport metrics update the physical frame size', {
  skip: prerequisiteSkip(['host-debug', 'engine-debug', 'icu-debug', 'empty-page-kernel']),
  timeout: 60000,
}, () => {
  const script = path.join(os.tmpdir(), `skyline-resize-${process.pid}.txt`);
  fs.writeFileSync(script, '1000 64 64 1\n');
  try {
    const { result, logs, details, pixels } = runHost({
      key: 'skyline_resize',
      assets: paths.appAssets('empty-page'),
      width: 128,
      height: 96,
      dpr: 1,
      frames: 0,
      timeoutMs: 2500,
      args: ['--resize-script', script],
      entrypoint: false,
      spawnTimeoutMs: 30000,
      // The last frame has the resized 64x64 viewport.
      frameSize: 64 * 64 * 4,
    });
    assert.equal(result.status, 0, `resize host failed:\n${logs}`);
    assert.match(logs, /\[host\] resize: at=1000 64x64 dpr=1 -> 64x64/);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
    assert.equal(details.width, 64);
    assert.equal(details.height, 64);
    assert.equal(details.frameSize, 64 * 64 * 4);
    assert.ok(pixels, 'no frame was written to shared memory');
    assert.deepEqual([...pixels.subarray(0, 4)], [255, 0, 0, 255]);
  } finally {
    fs.rmSync(script, { force: true });
  }
});

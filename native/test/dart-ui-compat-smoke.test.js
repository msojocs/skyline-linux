'use strict';

// Phase-3 runtime smoke test for the Skyline `dart:ui` compatibility layer.
//
// The page exercises Application, WindowStatus, FramePhase.vsyncTarget,
// createNativeParagraph, warmUpFont, the SceneBuilder/PlatformDispatcher
// additions and the private text flags, then renders a green frame with two
// span-background paragraphs.  A green first pixel proves the engine was
// rebuilt from the patched platform SDK (the kernel alone cannot prove that
// the compat classes exist in the core snapshot); the paragraph pixels prove
// `backgroundUseLineMetrics` reaches SkParagraph.
//
// Skips when the engine, host, patched SDK or compiled smoke app is missing.
// Set SKYLINE_REQUIRE_ENGINE=1 to fail instead of skipping in CI.

const test = require('node:test');
const assert = require('node:assert/strict');
const { paths, pixelAt: framePixelAt, prerequisiteSkip, runHost } = require('../../test/lib/host');

const width = 64;
const height = 48;
const expectedFrameSize = width * height * 4;

test('dart:ui compat: engine runs Application and renders a green frame', {
  skip: prerequisiteSkip(['host-debug', 'engine-debug', 'icu-debug', 'patched-sdk', 'ui-compat-kernel']),
  timeout: 60000,
}, () => {
  const { result, logs, details, pixels } = runHost({
    key: 'ui_compat',
    assets: paths.appAssets('dart-ui-compat-smoke'),
    width,
    height,
    entrypoint: false,
    noShutdown: false,
    spawnTimeoutMs: 30000,
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
  assert.equal(details.width, width);
  assert.equal(details.height, height);
  assert.equal(details.frameSize, expectedFrameSize);

  // stdout is flattened by the engine log callback; assert the Application
  // values survived the call.
  assert.match(result.stderr, /compat-smoke id=0 default=true stored=42 created=yes/);
  assert.match(result.stderr, /refreshRate=60\.0 parallel=false status=WindowStatus\.normal/);
  assert.match(result.stderr, /vsyncTarget=11/);
  assert.match(result.stderr, /windowStatus=WindowStatus\.normal/);

  assert.ok(pixels, 'no frame was written to shared memory');
  const pixelAt = (x, y) => framePixelAt(pixels, width, x, y);
  assert.deepEqual(pixelAt(0, 0), [0, 255, 0, 255], 'first pixel must be opaque green');
  assert.deepEqual(
    pixelAt(Math.floor(width / 2), Math.floor(height / 2)),
    [0, 255, 0, 255],
    'center pixel must be opaque green',
  );

  // The smoke app draws two 40px-tall line boxes (strut) with 10px runs and
  // a blue span background.  Only `backgroundUseLineMetrics: true` fills the
  // whole line box; the default path stays within the run's font metrics.
  assert.deepEqual(
    pixelAt(8, 10),
    [0, 0, 255, 255],
    'backgroundUseLineMetrics must paint the top of the line box',
  );
  assert.deepEqual(
    pixelAt(8, 44),
    [0, 0, 255, 255],
    'backgroundUseLineMetrics must paint the bottom of the line box',
  );
  assert.deepEqual(
    pixelAt(40, 10),
    [0, 255, 0, 255],
    'default background must not cover the line box',
  );
  assert.deepEqual(
    pixelAt(40, 44),
    [0, 255, 0, 255],
    'default background must not cover the line box',
  );
  // The unflagged run still paints its tight background near the baseline;
  // scan the column so the assertion does not depend on font metrics.
  let tightBackground = false;
  for (const x of [40, 41]) {
    for (let y = 8; y < 48; y++) {
      const pixel = pixelAt(x, y);
      if (pixel[2] > 100 && pixel[0] < 50 && pixel[1] < 50) {
        tightBackground = true;
      }
    }
  }
  assert.ok(
    tightBackground,
    'default background should still paint near the run',
  );
});

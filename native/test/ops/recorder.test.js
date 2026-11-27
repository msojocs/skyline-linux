'use strict';

// Appservice op replay: The real renderer bundle captured by tools/record-skyline-ops.js (needs the DevTools resources).
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {
  checkOpsBuild,
  countColor,
  legacyBundle,
  opsSkip,
  pixelAt,
  recordRealOps,
  runOps,
} = require('./helpers');

checkOpsBuild('appservice ops (recorder)');

test('appservice ops: real exparser bundle renders through the V8 capture', {
  skip: opsSkip,
  timeout: 90000,
}, () => {
  const recorded = recordRealOps();
  if (recorded === null) {
    // The proprietary renderer bundle is not part of the repository.
    return;
  }
  try {
    assert.ok(recorded.payload.ops.length > 50,
      `captured only ${recorded.payload.ops.length} operations`);
    const operations = new Set(recorded.payload.ops.map((op) => op.op));
    for (const required of ['createElement', 'appendChild', 'appendStyleSheet',
      'attach', 'startRender']) {
      assert.ok(operations.has(required), `captured stream lacks ${required}`);
    }

    const { result, logs, pixels } = runOps(recorded.path);
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /appservice ops: \d+ ops, \d+ nodes -> CreatePageRoute/);
    assert.ok(pixels, 'no frame was captured');

    // The example page from the official renderer bundle: a #ccc actions row
    // with two white buttons (rotate / translate), Latin glyphs, and a
    // 100x100 rgb(91,171,110) rect clipped by the viewport.
    const gray = countColor(pixels, (r, g, b) => r === 204 && g === 204 && b === 204);
    const green = countColor(pixels, (r, g, b) => r === 91 && g === 171 && b === 110);
    const white = countColor(pixels, (r, g, b) => r === 255 && g === 255 && b === 255);
    const dark = countColor(pixels, (r, g, b) => r < 100 && g < 100 && b < 100);
    assert.ok(gray > 3000, `expected the actions row background, found ${gray} pixels`);
    assert.ok(green > 1000, `expected the green rect, found ${green} pixels`);
    assert.ok(white > 5000, `expected the white buttons/page, found ${white} pixels`);
    assert.ok(dark > 100, `expected rendered text glyphs, found ${dark} dark pixels`);
  } finally {
    try {
      fs.unlinkSync(recorded.path);
    } catch {
      // Missing file is fine.
    }
  }
});

test('appservice ops: a captured tap gesture drives a real update batch', {
  skip: opsSkip,
  timeout: 90000,
}, () => {
  const recorded = recordRealOps({ tapOption: 1 });
  if (recorded === null) {
    // The proprietary renderer bundle is not part of the repository.
    return;
  }
  try {
    const updateBatches = recorded.payload.updates || [];
    assert.equal(updateBatches.length, 1, 'expected one post-attach update batch');
    const updateOps = updateBatches[0].ops;
    const kinds = new Set(updateOps.map((op) => op.op));
    assert.ok(kinds.has('setStyle'), 'tap should change the action-item background');
    assert.ok(kinds.has('addClass'), 'tap should add the rect transform class');

    const { result, logs, pixels, details } = runOps(recorded.path, {
      timeoutMs: 2000,
      frames: 0,
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /appservice update in \d+ ms/);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
    assert.ok(details.presentCount >= 2, `expected a repaint after the tap`);
    assert.ok(pixels, 'no frame was captured');

    // The first action item becomes yellow (`background-color: yellow`).
    assert.deepEqual(pixelAt(pixels, 10, 80), [255, 255, 0, 255]);
    const yellow = countColor(pixels, (r, g, b) => r === 255 && g === 255 && b === 0);
    assert.ok(yellow > 500, `expected the tapped button to turn yellow, found ${yellow}`);
  } finally {
    try {
      fs.unlinkSync(recorded.path);
    } catch {
      // Missing file is fine.
    }
  }
});

test('appservice ops: recorder samples animated style worklets', {
  skip: opsSkip,
  timeout: 90000,
}, () => {
  const bundle = legacyBundle();
  if (bundle === null) return;  // official renderer bundle not installed
  const recorded = recordRealOps({
    bundle,
    tapText: 'Jump To PageB',
    animateMs: 300,
  });
  if (recorded === null) return;
  try {
    const animated = [];
    for (const batch of recorded.payload.updates || []) {
      for (const op of batch.ops || []) {
        if (op.op === 'addAnimatedStyle' && op.updates) {
          animated.push({ atMs: batch.afterMs, updates: op.updates });
        }
      }
    }
    assert.ok(animated.length >= 3,
      `expected sampled animated frames, got ${animated.length}`);
    const translateValues = animated
      .map((entry) => entry.updates.transform)
      .filter((transform) => Array.isArray(transform))
      .map((transform) => {
        const first = transform.find((item) => item && item.translateX !== undefined);
        return first ? Number(first.translateX) : null;
      })
      .filter((value) => value !== null);
    assert.ok(new Set(translateValues).size > 1,
      `expected a varying translateX timeline, got ${translateValues}`);

    const { result, logs, pixels } = runOps(recorded.path, {
      timeoutMs: 3400,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const replayed = [...logs.matchAll(
      /animated style #\d+ at 0ms: .*translateX\(([\d.]+)px\)/g,
    )].map((match) => Number(match[1]));
    assert.ok(new Set(replayed).size > 1,
      `expected replayed translateX frames, got ${replayed}`);
    assert.ok(pixels, 'no frame was captured');
  } finally {
    try {
      fs.unlinkSync(recorded.path);
    } catch {
      // Missing file is fine.
    }
  }
});

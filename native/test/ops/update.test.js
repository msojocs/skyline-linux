'use strict';

// Appservice op replay: Incremental updates, transitions and animated styles.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {
  checkOpsBuild,
  height,
  opsSkip,
  pixelAt,
  repoRoot,
  runOps,
  width,
} = require('./helpers');

checkOpsBuild('appservice ops (update)');

test('appservice ops: removing a background repaints without a rebuild', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-bgclear-'));
  const ops = path.join(dir, 'bgclear.ops.json');
  // A-Z highlight switching: the old letter loses its background while the new
  // one gains it.  The targeted update must clear an existing decorated box
  // instead of leaving the old colour behind.
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '.box{width:100px;height:100px;background:#FF0000;}' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'addClass', id: 3, prefix: 'box', name: 1 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      // Wait until the initial page is materialized; a 200 ms update could
      // race the first build and leave the decorated box at its old colour.
      { afterMs: 600, ops: [{ op: 'removeClass', id: 3, prefix: 'box' }] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, { timeoutMs: 2000, frames: 0 });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 50, 50), [255, 255, 255, 255]);
    assert.doesNotMatch(logs, /appservice flush: callbacks missing/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: post-attach updates rebuild through the runtime', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-update.ops.json');
  const { result, logs, pixels, details } = runOps(fixture, {
    timeoutMs: 2500,
    frames: 0,
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /appservice update in 200 ms/);
  assert.match(logs, /appservice update in 400 ms/);
  assert.match(logs, /appservice update in 600 ms/);
  assert.match(logs, /appservice flush:/);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
  assert.ok(details.presentCount >= 3, `expected update frames, presentCount=${details.presentCount}`);
  assert.ok(pixels, 'no frame was captured');

  // 200ms: inline style overrides .red -> black.
  // 400ms: blue stripe appended after green.
  // 600ms: green stripe removed; blue moves up.
  assert.deepEqual(pixelAt(pixels, 160, 0), [0, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 79), [0, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 80), [51, 102, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 159), [51, 102, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 160), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 239), [255, 255, 255, 255]);
});

test('appservice ops: targeted text updates use TextSpanUpdate* callbacks', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-text-update.ops.json');
  const { result, logs, pixels, details } = runOps(fixture, {
    timeoutMs: 2000,
    frames: 0,
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /appservice targeted update: \d+ nodes/);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
  // The host stops after the deadline; initial frame + at least one updated
  // frame must be presented.  Updates at 200/400 ms can coalesce into a single
  // presentation, and the content assertions below verify they were applied.
  assert.ok(details.presentCount >= 2,
            `expected repaints, presentCount=${details.presentCount}`);
  assert.ok(pixels, 'no frame was captured');

  // "Hello" (red, 20px) -> "Updated text" (blue, 30px), both content and style
  // changes flowing through TextSpanUpdateText/TextSpanUpdateTextColor/FontSize.
  let blue = 0;
  let minX = width;
  let minY = height;
  let maxX = 0;
  let maxY = 0;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const offset = (y * width + x) * 4;
      const [r, g, b] = [pixels[offset], pixels[offset + 1], pixels[offset + 2]];
      if (b > 150 && r < 100 && g < 100) {
        blue++;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
      }
    }
  }
  assert.ok(blue > 400, `expected blue updated text, found ${blue} pixels`);
  assert.ok(maxX - minX > 150, `updated text bbox width ${maxX - minX} looks too small`);
  assert.ok(maxY < 60, `updated text bbox ends at ${maxY}`);
});

test('appservice ops: animated transforms republish hit-test rects', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // `applyAnimatedStyle` translates the half-screen sheet with
  // `transform: translateY(...)` (a paint-only targeted update, no rebuild).
  // The published rect snapshot must follow it, otherwise the sheet's hit
  // area stays at its closed position and taps inside the open sheet (the
  // close button, the comment list) all miss.
  const ops = [
    { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
    { op: 'setStyle', id: 2, value: 'width:320px;height:240px;background:#FFFFFF;' },
    { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 3,
      value: 'position:absolute;left:0;top:0;width:320px;height:240px;' +
             'background:#FF0000;transform:translateY(100%);' },
    { op: 'appendChild', id: 2, child: 3 },
    { op: 'attach', root: 2 },
    { op: 'startRender' },
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-anim-rect-'));
  const fixture = path.join(dir, 'anim-rect.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({
    ops,
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3,
          value: 'position:absolute;left:0;top:0;width:320px;height:240px;' +
                 'background:#FF0000;transform:translateY(0px);' },
      ] },
    ],
  }));
  try {
    const { result, logs } = runOps(fixture, {
      timeoutMs: 1200,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const tops = [...logs.matchAll(
      /\[ops-debug\] rect #3 left=([-\d.]+) top=([-\d.]+)/g,
    )].map((match) => Number(match[2]));
    assert.ok(tops.length >= 2,
      `expected rect publishes before and after the transform update:\n${logs}`);
    assert.equal(tops[0], 240,
      'the closed sheet is published at translateY(100%)');
    assert.equal(tops[tops.length - 1], 0,
      'the paint-only transform update republishes the open position');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: radius and root style updates stay targeted', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-radius-'));
  const ops = path.join(dir, 'radius.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:50px;top:50px;width:100px;height:100px;' +
          'background:#00AA00;border-radius:0px;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3, value: 'border-radius:50px;' },
      ] },
      { afterMs: 600, ops: [
        { op: 'setStyle', id: 2, value: 'background:#FF0000;' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2800,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(!/appservice flush:/.test(logs),
      `radius/root updates should stay targeted:\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // Rounded corner is transparent to the now-red page background.
    assert.deepEqual(pixelAt(pixels, 55, 55), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 100, 100), [0, 170, 0, 255]);
    // Root/page style update is visible outside the box.
    assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: CSS transitions interpolate before the final frame', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transition.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 3000,
    frames: 0,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /transition #3 start: mask=0x3 duration=400ms/);
  assert.match(logs, /transition #3 t=0\.[0-9]+ opacity=0\.[0-9]+ background=/);
  assert.match(logs, /transition #3 done/);
  assert.ok(pixels, 'no frame was captured');
  // 400ms later: blue at 50% opacity over the white page.
  assert.deepEqual(pixelAt(pixels, 80, 80), [127, 127, 255, 255]);
});

test('appservice ops: addAnimatedStyle timelines replay', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-animated-style.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 3000,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /addAnimatedStyle: #3/);
  assert.match(logs, /animated style scheduled: #3 in 200ms/);
  assert.match(logs, /animated style #3 at 200ms: background-color:#00FF00;opacity:0.5;/);
  assert.match(logs, /animated style #3 at 600ms: background-color:#0000FF;opacity:1;/);
  assert.ok(pixels, 'no frame was captured');
  assert.deepEqual(pixelAt(pixels, 80, 80), [0, 0, 255, 255]);
});

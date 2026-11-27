'use strict';

// Appservice op replay: Backgrounds, borders, gradients, transforms and clipping.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {
  checkOpsBuild,
  opsSkip,
  pixelAt,
  repoRoot,
  runOps,
} = require('./helpers');

checkOpsBuild('appservice ops (visual)');

test('appservice ops: CSS transforms paint without affecting flex flow', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // translate(100px, 50px): the 100x100 green box keeps its flow slot but
  // paints at (100,50).
  assert.deepEqual(pixelAt(pixels, 10, 10), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 150, 100), [0, 170, 0, 255]);
  // scale(0.5) around the centre of the second flow child (y=100..200).
  assert.deepEqual(pixelAt(pixels, 50, 150), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 150), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 90), [255, 255, 255, 255]);
});

test('appservice ops: CSS box-shadow offsets paint behind the node', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-shadow.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  assert.deepEqual(pixelAt(pixels, 60, 60), [0, 170, 0, 255]);
  // box-shadow: 10px 10px 0 rgba(0,0,0,.5) -> 50% black over white.
  assert.deepEqual(pixelAt(pixels, 125, 60), [128, 128, 128, 255]);
  assert.deepEqual(pixelAt(pixels, 60, 125), [128, 128, 128, 255]);
  assert.deepEqual(pixelAt(pixels, 125, 125), [128, 128, 128, 255]);
  assert.deepEqual(pixelAt(pixels, 200, 200), [255, 255, 255, 255]);
});

test('appservice ops: CSS gradients build and update in place', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-gradient-'));
  const ops = path.join(dir, 'gradient.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:50px;top:50px;width:100px;height:100px;' +
          'background:linear-gradient(90deg,#FF0000,#0000FF);' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3,
          value: 'background:linear-gradient(90deg,#00FF00,#FFFF00);' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    const left = pixelAt(pixels, 55, 100);
    const right = pixelAt(pixels, 145, 100);
    assert.ok(left[1] > 200 && left[0] < 50 && left[2] < 50,
      `expected green on the left, got ${left}`);
    assert.ok(right[0] > 200 && right[1] > 200 && right[2] < 50,
      `expected yellow on the right, got ${right}`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: transform-origin changes the transform pivot', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform-origin.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // scale(0.5) around left/top keeps the box at its top-left corner.
  assert.deepEqual(pixelAt(pixels, 65, 25), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 130, 60), [255, 255, 255, 255]);
  // scale(0.5) around right/bottom anchors the bottom-right corner.
  assert.deepEqual(pixelAt(pixels, 295, 115), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 205, 25), [255, 255, 255, 255]);
});

test('appservice ops: per-side borders render and update', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-borders-'));
  const ops = path.join(dir, 'borders.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:100px;top:100px;width:100px;height:100px;' +
          'background:#FFFFFF;border-top:10px solid #FF0000;' +
          'border-left:20px solid #0000FF;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3,
          value: 'border-top-color:#00AA00;border-left-width:0px;' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // Top border is green after the update; the left border was removed.
    assert.deepEqual(pixelAt(pixels, 150, 105), [0, 170, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 105, 150), [255, 255, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 150, 150), [255, 255, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: overflow hidden clips children to the border box', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-overflow.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // child starts at (150,150) and is clipped at the 100x100 box.
  assert.deepEqual(pixelAt(pixels, 120, 120), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 160), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 220, 120), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 120, 220), [255, 255, 255, 255]);
  // rounded clip: the overflowing child is cut by the 50px radius.
  assert.deepEqual(pixelAt(pixels, 225, 105), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 270, 150), [255, 0, 0, 255]);
});

test('appservice ops: per-corner border-radius updates', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-corners-'));
  const ops = path.join(dir, 'corners.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:100px;top:100px;width:100px;height:100px;' +
          'background:#00AA00;border-radius:50px 0 0 50px;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3, value: 'border-radius:0 50px 50px 0;' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // Left corners are square after the update; right corners are rounded.
    assert.deepEqual(pixelAt(pixels, 105, 105), [0, 170, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 195, 105), [255, 255, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 105, 195), [0, 170, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 195, 195), [255, 255, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: skew and 3D transforms paint', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform3d.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // skewX(45deg) shifts the top edge left and the bottom edge right.
  assert.deepEqual(pixelAt(pixels, 80, 30), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 5, 30), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 150, 110), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 50, 110), [255, 255, 255, 255]);
  // perspective(200px) rotateY(60deg) narrows the 100px blue box around its
  // centre; the outer edge is now background.
  assert.deepEqual(pixelAt(pixels, 250, 70), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 210, 70), [255, 255, 255, 255]);
  // skewY(45deg) shifts the left edge up and the right edge down.
  assert.deepEqual(pixelAt(pixels, 80, 150), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 140, 150), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 100, 200), [255, 0, 0, 255]);
});

test('appservice ops: radial and conic gradients paint through NewDecoratedBox', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-radial-conic.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  const isRed = ([r, g, b]) => r > 240 && g < 20 && b < 20;
  const isGreen = ([r, g, b]) => r < 40 && g > 240 && b < 40;
  const isBlue = ([r, g, b]) => r < 40 && g < 40 && b > 240;
  const isYellow = ([r, g, b]) => r > 240 && g > 240 && b < 40;
  const isCyan = ([r, g, b]) => r < 40 && g > 240 && b > 240;
  const isMagenta = ([r, g, b]) => r > 240 && g < 40 && b > 240;

  // radial-gradient(circle at center, red 0%, blue 100%): the centre is red
  // and the colour walks toward blue with distance.
  const r1Center = pixelAt(pixels, 60, 60);
  assert.ok(isRed(r1Center), `radial centre should be red, got ${r1Center}`);
  const r1Inner = pixelAt(pixels, 50, 50);
  assert.ok(r1Inner[0] > 180 && r1Inner[2] > 30 && r1Inner[2] < 80,
    `radial quarter point should be red/blue blend, got ${r1Inner}`);

  // radial-gradient(circle 30px at 50% 50%, green, yellow): the 30px radius
  // reaches yellow while the centre stays green.
  const r2Center = pixelAt(pixels, 180, 60);
  assert.ok(isGreen(r2Center), `30px radial centre should be green, got ${r2Center}`);
  assert.ok(isYellow(pixelAt(pixels, 210, 50)),
    'points beyond the 30px radius should be yellow');

  // conic-gradient(from 0deg, red/green/blue/yellow quadrants): CSS angles run
  // clockwise from 12 o'clock.
  assert.ok(isRed(pixelAt(pixels, 90, 130)), 'conic upper-right is red');
  assert.ok(isGreen(pixelAt(pixels, 90, 210)), 'conic lower-right is green');
  assert.ok(isBlue(pixelAt(pixels, 50, 210)), 'conic lower-left is blue');
  assert.ok(isYellow(pixelAt(pixels, 50, 130)), 'conic upper-left is yellow');

  // conic-gradient(from 90deg, magenta/cyan halves): rotating the start angle
  // by 90 degrees moves the half-turn boundary to the top/bottom axis.
  assert.ok(isCyan(pixelAt(pixels, 210, 170)), 'rotated conic upper-right is cyan');
  assert.ok(isMagenta(pixelAt(pixels, 170, 210)), 'rotated conic lower-left is magenta');

  assert.deepEqual(pixelAt(pixels, 120, 120), [255, 255, 255, 255],
    'gap between gradient boxes stays white');
});

test('appservice ops: radial gradients update in place to conic', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-gradient-kind-'));
  const ops = path.join(dir, 'gradient-kind.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:50px;top:50px;width:100px;height:100px;' +
          'background:radial-gradient(circle at center,#FF0000 0%,#0000FF 100%);' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3,
          value: 'background:conic-gradient(from 0deg at 50% 50%,' +
            '#00FF00 0deg 180deg,#FF00FF 180deg 360deg);' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // The same DecoratedBox updates in place: upper-right half green,
    // lower-left magenta (the original radial red/blue is gone).
    assert.deepEqual(pixelAt(pixels, 130, 80), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 70, 120), [255, 0, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: transform-origin z moves the 3D pivot', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform-origin-z.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // Both boxes share `perspective(300px) rotateX(40deg)`.  The left box pivots
  // at z=0 (its projection starts around y=118); the right box pivots 60px
  // toward the viewer, which shifts the same projection about 32px down.
  assert.deepEqual(pixelAt(pixels, 60, 125), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 230, 125), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 60, 185), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 230, 185), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 230, 210), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 60, 210), [255, 255, 255, 255]);
});

test('appservice ops: transform-origin z updates repaint in place', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-origin-z-'));
  const ops = path.join(dir, 'origin-z.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:20px;top:100px;width:100px;height:100px;' +
          'background:#00AA00;transform:perspective(300px) rotateX(40deg);' +
          'transform-origin:50% 50% 0px;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3, value: 'transform-origin:50% 50% 60px;' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // After moving the pivot to z=60 the projection drops like the right-hand
    // box in the committed fixture.
    assert.deepEqual(pixelAt(pixels, 60, 125), [255, 255, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 185), [0, 170, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 210), [0, 170, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

'use strict';

// Appservice op replay: Box model, flexbox and positioning.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
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

checkOpsBuild('appservice ops (layout)');

test('appservice ops: committed stripe fixture replays through the runtime', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-stripes.ops.json');
  const { result, logs, pixels } = runOps(fixture);

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /appservice ops: 17 ops, 5 nodes -> CreatePageRoute/);
  assert.ok(pixels, 'no frame was captured');

  // Page root is 320x240; three 80px flex-column children stretch to full
  // width.  The third child uses inline style instead of a CSS class.
  assert.deepEqual(pixelAt(pixels, 160, 0), [255, 51, 102, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 79), [255, 51, 102, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 80), [51, 204, 102, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 159), [51, 204, 102, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 160), [51, 102, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 239), [51, 102, 255, 255]);

  // Deterministic input -> deterministic software-rendered frame.  This is the
  // Linux half of the future L5 comparison; the Windows side still needs the
  // official appservice guest (docs/feasibility.md section 3).  The host runs
  // the JIT kernel with asserts, so the app compat patch disables Flutter's
  // debug banner; the frame is three solid bands and does not depend on the
  // installed fonts.
  assert.equal(
    crypto.createHash('sha256').update(pixels).digest('hex'),
    '2fe6f9b8d124c107c14f87df631551a90eaf3a92d1f6305f4bddb7a5bb951461',
  );
});

test('appservice ops: absolute boxes honour Yoga leading margins', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors expanded-scroll-view's tab indicator:
  //   .tabs-indicator{position:absolute;left:0;bottom:0;width:20px;
  //                   height:2px;margin:0 21px;background:orange}
  // The official Windows engine (Yoga) adds the *leading* margin to every
  // absolutely positioned box, subtracts both margins only when `left`+`right`
  // determine the size, and ignores the trailing margin for right/bottom
  // anchoring.  The expected boxes below are the official Windows engine's
  // pixels (dpr=2 device coordinates, captured via tools/l5-windows.js).
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-absolute-margin.ops.json');
  const { result, logs, pixels } = runOps(fixture, { dpr: 2 });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  const frameWidth = width * 2;
  const bbox = (r, g, b) => {
    let x0 = Infinity; let y0 = Infinity; let x1 = -1; let y1 = -1;
    for (let y = 0; y < height * 2; y++) {
      for (let x = 0; x < frameWidth; x++) {
        const offset = (y * frameWidth + x) * 4;
        if (pixels[offset] === r && pixels[offset + 1] === g &&
            pixels[offset + 2] === b) {
          if (x < x0) x0 = x;
          if (y < y0) y0 = y;
          if (x > x1) x1 = x;
          if (y > y1) y1 = y;
        }
      }
    }
    return [x0, y0, x1, y1];
  };

  // left:0 + margin-left:21 on a 20px bar -> x 21..41 CSS (42..81 device).
  assert.deepEqual(bbox(255, 165, 0), [42, 396, 81, 399]);
  // right:0 + width:30 + margin-left:10 -> x 300..330, clipped at the 320px
  // viewport; margin-right is not subtracted.
  assert.deepEqual(bbox(0, 0, 255), [600, 200, 639, 215]);
  // bottom:0 + height:20 + margin-top:10 -> y 190..210; margin-bottom ignored.
  assert.deepEqual(bbox(0, 255, 255), [240, 380, 279, 419]);
  // left:10 + right:10 + margin:0 5px -> x 15..305 with both margins removed.
  assert.deepEqual(bbox(0, 170, 0), [30, 240, 609, 255]);
  // No insets: static position + leading margins -> (40,30)..(52,42).
  assert.deepEqual(bbox(255, 0, 255), [80, 60, 103, 83]);
});

test('appservice ops: defaultContentBox renderer flag stacks height and padding', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // app-bar's navigation bar declares `height: navBarHeight` and
  // `padding-top: statusBarHeight`; with the project's
  // rendererOptions.skyline.defaultContentBox the box is height+padding tall
  // (the SDK passes the flag to `new SkylineGlobal.PageContext(id, windowId,
  // flags)`).  Without it the 42px height is consumed by the 47px padding and
  // the title is clipped.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-content-box.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    dpr: 2,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // Windows oracle: red nav 0..89 CSS (42 + 47), blue block from 89.
  assert.match(logs, /rect #3 left=0 top=0 w=320 h=89/);
  const pixelAt2 = (x, y) => {
    const offset = (y * width * 2 + x) * 4;
    return [...pixels.subarray(offset, offset + 4)];
  };
  assert.deepEqual(pixelAt2(600, 170), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt2(600, 184), [0, 0, 255, 255]);
});

test('appservice ops: flex items clamp their cross size to the line', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors app-bar's navigation bar: `.nav{display:flex}` (column),
  // `.inner{width:100vw; padding-right}` and a flex:1 centre.  The official
  // Windows engine clamps the oversized flex item's border box to the line
  // width, so the centre spans 96..240 (not 96..320) and the title sits where
  // the real DevTools puts it.  Verified pixel-identically by
  // `appservice-content-box-nav` in test:l5:windows.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-content-box-nav.ops.json');
  const { result, logs } = runOps(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  // Content-box height: 42 + 47 padding.
  assert.match(logs, /rect #4 left=0 top=0 w=320 h=89/);
  // Left slot width 80 + padding-left 16.
  assert.match(logs, /rect #5 left=0 top=47 w=96 h=42/);
  // Centre clamped to the line: 320 - 80 padding - 96 left slot = 144.
  assert.match(logs, /rect #6 left=96 top=47 w=144 h=42/);
});

test('appservice ops: calc() and env() resolve in box edges', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // app-bar's `.scroll-area` reserves room for the mini player with
  // `margin-bottom: calc(84px + env(safe-area-inset-bottom))`; dropping the
  // declaration let the list run under the app bar and hid the last cards.
  // Verified against the Windows engine (`appservice-calc-env`, inset 0 here).
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-calc-env.ops.json');
  const { result, logs } = runOps(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /rect #3 left=0 top=0 w=320 h=120/);
  // 120 + calc(40px + 0px) env bottom.
  assert.match(logs, /rect #4 left=0 top=160 w=320 h=20/);
});

test('appservice ops: stretch cross size subtracts cross-axis margins', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors tab-indicator's `.tab-list { margin: 5px 24px 0 }` inside a
  // column page: align-items:stretch sizes the margin box, so the item is
  // 320-48=272 wide and starts at x=24 (the tab indicator uses 1/3 of it).
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-stretch-margin-'));
  const fixture = path.join(dir, 'stretch.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1, css:
        '.page { width: 320px; height: 240px; } ' +
        '.col { width: 320px; height: 240px; display: flex; ' +
        'flex-direction: column; } ' +
        '.row { margin: 5px 24px 0; height: 50px; display: flex; ' +
        'flex-direction: row; }' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'addClass', id: 2, prefix: 'page', name: 1 },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'addClass', id: 3, prefix: 'col', name: 1 },
      { op: 'createElement', id: 4, tag: 'empty', name: 'view' },
      { op: 'addClass', id: 4, prefix: 'row', name: 1 },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs } = runOps(fixture, {
      timeoutMs: 8000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const match = new RegExp(
      '\\[ops-debug\\] rect #4 left=([-\\d.]+) top=([-\\d.]+) ' +
      'w=([-\\d.]+) h=([-\\d.]+)').exec(logs);
    assert.ok(match, `no rect published for node #4:\n${logs}`);
    assert.equal(Number(match[1]), 24);   // margin-left
    assert.equal(Number(match[2]), 5);    // margin-top
    assert.equal(Number(match[3]), 272);  // 320 - 24 - 24
    assert.equal(Number(match[4]), 50);
  } finally {
    try {
      fs.rmSync(dir, { recursive: true, force: true });
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: absolute nodes use the parent content box', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-absolute.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // Red flow child is unaffected by the three absolute siblings.
  assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255]);
  // right:20 top:20 width:50 height:50
  assert.deepEqual(pixelAt(pixels, 260, 30), [0, 0, 255, 255]);
  // left:10 bottom:10 width:100 height:60
  assert.deepEqual(pixelAt(pixels, 20, 180), [0, 170, 0, 255]);
  // left:20 right:20 top:130 height:20 stretches horizontally
  assert.deepEqual(pixelAt(pixels, 160, 140), [255, 255, 0, 255]);
  // Parent background outside the absolute boxes.
  assert.deepEqual(pixelAt(pixels, 200, 200), [238, 238, 238, 255]);
});

test('appservice ops: align-self and per-axis gaps', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-align-gap.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // row: column-gap 20, align-self center/end.
  assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 60, 25), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 80, 50), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 80, 10), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 150, 80), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 150, 10), [255, 255, 255, 255]);
  // column: row-gap 30 between the 30px children.
  assert.deepEqual(pixelAt(pixels, 10, 130), [255, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 160), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 190), [0, 255, 255, 255]);
});

test('appservice ops: CSS order reorders flow children', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-order.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // Source order a,b,c with order 2,0,1 -> visual order b,c,a.
  assert.deepEqual(pixelAt(pixels, 25, 25), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 75, 25), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 125, 25), [255, 0, 0, 255]);
});

test('appservice ops: flex-wrap breaks rows and columns', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-flex-wrap.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // row wrap: two 90px children fit in 200px, the third moves to line 2.
  assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 110, 10), [255, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 60), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 110, 60), [255, 255, 255, 255]);
  // column wrap: two 40px children fit in 100px, the third starts column 2.
  assert.deepEqual(pixelAt(pixels, 230, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 230, 60), [255, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 262, 30), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 262, 60), [255, 255, 255, 255]);
});

test('appservice ops: position fixed resolves against the viewport', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-fixed.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // flow child inside the offset parent.
  assert.deepEqual(pixelAt(pixels, 80, 80), [255, 0, 0, 255]);
  // fixed right:10 bottom:10 -> viewport (270..309, 200..229), not the parent
  // content box at (70,70).
  assert.deepEqual(pixelAt(pixels, 285, 215), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 200, 100), [221, 221, 221, 255]);
  // fixed left:10 top:10 -> viewport corner.
  assert.deepEqual(pixelAt(pixels, 15, 15), [0, 170, 0, 255]);
});

test('appservice ops: flex-basis and flex-shrink size items', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-flex-basis.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // Row 1: `0 0 110px` + `1 1 0%` + `2 1 0px` over 320px.
  assert.deepEqual(pixelAt(pixels, 109, 35), [255, 0, 0, 255], 'flex-basis px');
  assert.deepEqual(pixelAt(pixels, 110, 35), [0, 170, 0, 255], 'grow 1 starts');
  assert.deepEqual(pixelAt(pixels, 179, 35), [0, 170, 0, 255], 'grow 1 ends');
  assert.deepEqual(pixelAt(pixels, 180, 35), [0, 0, 255, 255], 'grow 2 starts');

  // Row 2: two 150px bases shrink by 25/75 (1:3) into a 200px row.
  assert.deepEqual(pixelAt(pixels, 124, 120), [255, 255, 0, 255], 'shrink 1');
  assert.deepEqual(pixelAt(pixels, 125, 120), [255, 0, 255, 255], 'shrink 3');
  assert.deepEqual(pixelAt(pixels, 199, 120), [255, 0, 255, 255], 'shrink end');
  assert.deepEqual(pixelAt(pixels, 200, 120), [255, 255, 255, 255], 'row width');

  // Row 3: `0 0 25%` resolves against the 320px row; the auto-basis grow item
  // takes the rest.
  assert.deepEqual(pixelAt(pixels, 79, 200), [0, 255, 255, 255], '25% basis');
  assert.deepEqual(pixelAt(pixels, 80, 200), [255, 136, 0, 255], 'auto basis');
  assert.deepEqual(pixelAt(pixels, 319, 200), [255, 136, 0, 255], 'grow end');
});

test('appservice ops: box-sizing content-box adds padding and border', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-box-sizing.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // content-box: 100x60 content + 10px padding + 5px border -> 130x90.
  assert.deepEqual(pixelAt(pixels, 2, 2), [0, 0, 0, 255], 'outer border');
  assert.deepEqual(pixelAt(pixels, 20, 20), [0, 0, 255, 255], 'child at content origin');
  assert.deepEqual(pixelAt(pixels, 40, 40), [255, 0, 0, 255], 'content box');
  assert.deepEqual(pixelAt(pixels, 120, 80), [255, 0, 0, 255], 'padding area');
  assert.deepEqual(pixelAt(pixels, 128, 40), [0, 0, 0, 255], 'right border');
  assert.deepEqual(pixelAt(pixels, 60, 88), [0, 0, 0, 255], 'bottom border');
  assert.deepEqual(pixelAt(pixels, 60, 92), [255, 255, 255, 255], 'content-box ends at 90');
  assert.deepEqual(pixelAt(pixels, 140, 10), [255, 255, 255, 255], 'gap');

  // border-box: 100x60 total; content is 70x30 starting at (15,15).
  assert.deepEqual(pixelAt(pixels, 162, 2), [0, 0, 0, 255], 'border-box outer border');
  assert.deepEqual(pixelAt(pixels, 180, 20), [0, 0, 255, 255], 'child at content origin');
  assert.deepEqual(pixelAt(pixels, 200, 40), [255, 0, 0, 255], 'border-box content');
  assert.deepEqual(pixelAt(pixels, 250, 40), [255, 0, 0, 255], 'padding area');
  assert.deepEqual(pixelAt(pixels, 200, 58), [0, 0, 0, 255], 'bottom border');
  assert.deepEqual(pixelAt(pixels, 200, 62), [255, 255, 255, 255], 'border-box ends at 60');
  assert.deepEqual(pixelAt(pixels, 270, 70), [255, 255, 255, 255], 'right of border-box');
});

test('appservice ops: percentage padding and margin use containing width', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-percent-box.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // padding:10% -> 32px on all sides (10% of the 320px containing block width).
  assert.deepEqual(pixelAt(pixels, 2, 2), [255, 0, 0, 255], 'padding area');
  assert.deepEqual(pixelAt(pixels, 33, 33), [0, 0, 255, 255], 'child at padded content origin');
  assert.deepEqual(pixelAt(pixels, 40, 40), [0, 0, 255, 255], 'child body');
  assert.deepEqual(pixelAt(pixels, 60, 60), [255, 0, 0, 255], 'content background');
  assert.deepEqual(pixelAt(pixels, 199, 99), [255, 0, 0, 255], 'border-box edge');
  assert.deepEqual(pixelAt(pixels, 210, 10), [255, 255, 255, 255], 'outside');

  // margin:10% 25% -> 20px vertical / 50px horizontal (10%/25% of 200px
  // containing width, not the 40px height).
  assert.deepEqual(pixelAt(pixels, 55, 145), [255, 255, 0, 255], 'margin offsets child');
  assert.deepEqual(pixelAt(pixels, 10, 130), [0, 170, 0, 255], 'margin left');
  assert.deepEqual(pixelAt(pixels, 100, 130), [0, 170, 0, 255], 'margin top');
  assert.deepEqual(pixelAt(pixels, 210, 130), [255, 255, 255, 255], 'outside margin box');
});

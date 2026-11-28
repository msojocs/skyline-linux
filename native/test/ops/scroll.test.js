'use strict';

// Appservice op replay: Scroll views, refresher, list/grid slivers, builders and sticky/fixed children.
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
} = require('./helpers');

checkOpsBuild('appservice ops (scroll)');

test('appservice ops: scrollview renders clipped content', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const { result, logs, pixels } = runOps(fixture);

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // 400px content inside a 120px scroll view: red 0..99, green 100..119,
  // clipped below the scroll view.
  assert.deepEqual(pixelAt(pixels, 160, 0), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 99), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 100), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 119), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 120), [255, 255, 255, 255]);
});

test('appservice ops: horizontal scroll-view lays items along the scroll axis', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors associated-scroll-view's `<scroll-view scroll-x type="list">`:
  // the renderer creates the inner `scrollview` element without the user's
  // `display:flex;flex-direction:row`, so the runtime derives the flow
  // direction from `scrollX`.  Intrinsic 70x90 items must sit side by side.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-scroll-horizontal.ops.json');
  const { result, logs, pixels } = runOps(fixture);

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // 70px items with a 50px swatch: red/green/blue/yellow in the viewport.
  assert.deepEqual(pixelAt(pixels, 35, 35), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 105, 35), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 175, 35), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 245, 35), [255, 255, 0, 255]);
  // Only the first row is painted; the old column layout stacked them.
  assert.deepEqual(pixelAt(pixels, 35, 105), [255, 255, 255, 255]);
});

test('appservice ops: scroll-view at its edge yields the drag to the parent swiper', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors associated-scroll-view: a vertical swiper whose current page is a
  // scroll-view.  Once the list is at the bottom, an upward drag must not be
  // consumed by the scroll-view; ShouldGestureResponse reports the yield and
  // package:skyline lets the swiper's own drag recognizer page.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-swiper-scroll-yield.ops.json');
  const script = path.join(
    os.tmpdir(), `skyline-swiper-yield-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1400 down 160 200 1',
    '1430 move 160 160 1',
    '1460 move 160 120 1',
    '1490 move 160 80 1',
    '1520 move 160 40 1',
    '1550 up 160 40 1',
    '',
  ].join('\n'));
  try {
    const { result, logs } = runOps(fixture, {
      timeoutMs: 3000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    // The fixture scrolls the list to its bottom before the drag.
    assert.match(logs, /ShouldGestureResponse: scroll #5 -> swiper #3 /);
    assert.match(logs, /ShouldGestureResponse: scroll #5 -> swiper #3 .*yield=1/);
    assert.match(logs, /swiper change: id=3 page=1/);
  } finally {
    try {
      fs.rmSync(script, { force: true });
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: ScrollViewUpdateScrollTop scrolls the content', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll.ops.json');
  const { result, logs, pixels } = runOps(fixture, { debugLogs: true, timeoutMs: 2000, frames: 0 });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /appservice setScrollTop: id=3 value=80/);
  assert.ok(pixels, 'no frame was captured');

  // After jumping to 80px, red is 0..19 and green starts at y=20.
  assert.deepEqual(pixelAt(pixels, 160, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 20), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 100), [0, 255, 0, 255]);
});

test('appservice ops: host drag gesture scrolls the scroll view', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const script = path.join(os.tmpdir(), `skyline-drag-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1400 add 160 100 1',
    '1500 down 160 100 1',
    '1600 move 160 90 1',
    '1700 move 160 80 1',
    '1800 move 160 70 1',
    '1900 move 160 60 1',
    '2000 up 160 60 1',
    '',
  ].join('\n'));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    // The Skyline vertical-drag recognizer wins the arena (ShouldAcceptGesture /
    // ShouldGestureResponse) and feeds the scroll position.  Recognizer handler
    // ids are the owning node ids (#3 is the scrollview).
    assert.match(logs, /ShouldGestureResponse: 3 -> 1/);
    assert.match(logs, /ShouldAcceptGesture: 3 -> 1/);
    assert.match(logs,
      /scroll callbacks installed #3 \(start=1 scroll=1 end=1\)/);
    assert.match(logs, /scroll start\/end: top=0 .*drag=1/);
    assert.match(logs, /scroll: top=[1-9]\d*(\.\d+)? /);
    assert.ok(pixels, 'no frame was captured');

    // 40px of pointer travel minus Flutter's touch slop scrolls the 400px
    // content; the red/green boundary moves up from y=100 by roughly slop
    // distance.
    let firstGreen = height;
    for (let y = 0; y < height; y++) {
      const [r, g] = pixelAt(pixels, 160, y);
      if (g > 200 && r < 100) {
        firstGreen = y;
        break;
      }
    }
    assert.ok(firstGreen > 60 && firstGreen < 100,
      `expected the drag to scroll by ~22px, red/green boundary at y=${firstGreen}`);
    assert.deepEqual(pixelAt(pixels, 160, 0), [255, 0, 0, 255]);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: mouse wheel scrolls the scroll view', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const script = path.join(os.tmpdir(), `skyline-wheel-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1200 add 160 60 1',
    '1500 scroll 160 60 0 80',
    '',
  ].join('\n'));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    // PointerScrollEvent is accepted because shouldAcceptUserOffset() reports
    // the content as scrollable.
    assert.match(logs, /scroll: top=80 .*delta=\(0,80\) drag=0/);
    assert.ok(pixels, 'no frame was captured');

    // A 80px wheel delta moves green up to y=20.
    assert.deepEqual(pixelAt(pixels, 160, 10), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 160, 20), [0, 255, 0, 255]);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: refresher programmatic pull drives callbacks', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-refresher-'));
  const ops = path.join(dir, 'refresher.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'scrollview' },
      { op: 'setStyle', id: 3, value: 'width:320px;height:120px;' },
      { op: 'setAttributes', id: 3, attrs: {
        scrollY: true, scrollEnabled: true, refresherEnabled: true,
        refresherThreshold: 50, refresherTriggered: false,
        refresherDefaultStyle: 'black',
      } },
      { op: 'createElement', id: 4, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 4, value: 'height:400px;' },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 800, ops: [
        { op: 'refresherTrigger', id: 3, open: true },
      ] },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 5000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /refresher trigger: id=3 open=1 twoLevel=0/);
    assert.match(logs, /refresher pulling: id=3 dy=[\d.]+/);
    assert.match(logs, /refresher status: id=3 mode=2 dy=[\d.]+/);
    assert.match(logs, /refresher refresh: id=3 dy=[\d.]+/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: list-view renders a virtualized sliver', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-listview.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /listview #4: 4 items/);
  assert.ok(pixels, 'no frame was captured');

  // Four 80px items inside a 120px scroll view: red 0..79, green 80..119,
  // clipped below the viewport.
  assert.deepEqual(pixelAt(pixels, 160, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 79), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 80), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 119), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 120), [255, 255, 255, 255]);

  // Mouse wheel scrolling works through the list sliver.
  const script = path.join(os.tmpdir(), `skyline-list-wheel-${process.pid}.txt`);
  fs.writeFileSync(script, ['1200 add 160 60 1', '1500 scroll 160 60 0 80', ''].join('\n'));
  try {
    const scrolled = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(scrolled.result.status, 0, scrolled.logs);
    assert.match(scrolled.logs, /scroll: top=80 .*delta=\(0,80\)/);
    // Red has scrolled out; green now starts at the top and blue follows.
    assert.deepEqual(pixelAt(scrolled.pixels, 160, 10), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(scrolled.pixels, 160, 20), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(scrolled.pixels, 160, 100), [0, 0, 255, 255]);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: grid-view lays out a 2-column aligned grid', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-gridview.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /gridview #4: 4 items/);
  assert.ok(pixels, 'no frame was captured');

  // Two columns of 160px: red/green on the first row, blue/yellow on the
  // second.
  assert.deepEqual(pixelAt(pixels, 80, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 240, 10), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 80, 90), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 240, 90), [255, 255, 0, 255]);
});

test('appservice ops: grid-view masonry packs columns without blank rows', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors the ops the SDK sends for `<grid-view type="masonry">`: camelCase
  // `type` string + `mainAxisGap`/`crossAxisGap` in a bulk setAttributes op.
  const ops = [
    { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
    { op: 'setStyle', id: 2, value: 'width:320px;height:240px;background:#FFFFFF;' },
    { op: 'createElement', id: 3, tag: 'empty', name: 'scrollview' },
    { op: 'setStyle', id: 3, value: 'width:320px;height:240px;' },
    { op: 'setAttributes', id: 3, attrs: { scrollY: true, scrollEnabled: true } },
    { op: 'createElement', id: 4, tag: 'empty', name: 'gridview' },
    { op: 'setAttributes', id: 4, attrs: { type: 'masonry' } },
    { op: 'setAttributes', id: 4, attrs: { crossAxisGap: 4, mainAxisGap: 4 } },
    { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 5, value: 'width:100%;height:100px;background:#FF0000;' },
    { op: 'createElement', id: 6, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 6, value: 'width:100%;height:60px;background:#00FF00;' },
    { op: 'createElement', id: 7, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 7, value: 'width:100%;height:100px;background:#0000FF;' },
    { op: 'createElement', id: 8, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 8, value: 'width:100%;height:100px;background:#FFFF00;' },
    { op: 'appendChild', id: 4, child: 5 },
    { op: 'appendChild', id: 4, child: 6 },
    { op: 'appendChild', id: 4, child: 7 },
    { op: 'appendChild', id: 4, child: 8 },
    { op: 'appendChild', id: 3, child: 4 },
    { op: 'appendChild', id: 2, child: 3 },
    { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
    { op: 'appendChild', id: 1, child: 2 },
    { op: 'attach', root: 1 },
    { op: 'startRender' },
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-masonry-'));
  const fixture = path.join(dir, 'masonry.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({ ops }));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /gridview #4: 4 items/);
    assert.ok(pixels, 'no frame was captured');
    // Published rects must match the painted masonry packing: red 0..99
    // (col 0), green 0..59 (col 1), blue 64..163 (col 1, shortest), yellow
    // 104..203 (col 0).  Previously every item was published at x=0, so the
    // right column could not be hit-tested.
    const rectOf = (id) => {
      const match = new RegExp(
        `\\[ops-debug\\] rect #${id} left=([-\\d.]+) top=([-\\d.]+) ` +
        'w=([-\\d.]+) h=([-\\d.]+)').exec(logs);
      assert.ok(match, `no rect published for node #${id}:\n${logs}`);
      return match.slice(1).map(Number);
    };
    assert.deepEqual(rectOf(5), [0, 0, 158, 100]);
    assert.deepEqual(rectOf(6), [162, 0, 158, 60]);
    assert.deepEqual(rectOf(7), [162, 64, 158, 100]);
    assert.deepEqual(rectOf(8), [0, 104, 158, 100]);
    // Masonry fills the shortest column: red 0..99 (col 0), green 0..59
    // (col 1), blue 64..163 (col 1, shortest), yellow 104..203 (col 0).  An
    // aligned grid would leave a blank 60..103 band in column 1 and start row
    // 1 at y=104 in both columns, so (240,80) would be white instead of blue.
    assert.deepEqual(pixelAt(pixels, 80, 30), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 30), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 80), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 80, 150), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 150), [0, 0, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: published grid rects follow the scroll offset', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Two masonry columns in a 200px-tall scroll view.  With scrollTop=100 the
  // published rects must be viewport coordinates, otherwise tapping a card
  // after scrolling still hits the content-space position.
  const ops = [
    { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
    { op: 'setStyle', id: 2, value: 'width:320px;height:200px;background:#FFFFFF;' },
    { op: 'createElement', id: 3, tag: 'empty', name: 'scrollview' },
    { op: 'setStyle', id: 3, value: 'width:320px;height:200px;' },
    { op: 'setAttributes', id: 3,
      attrs: { scrollY: true, scrollEnabled: true, scrollTop: 100 } },
    { op: 'createElement', id: 4, tag: 'empty', name: 'gridview' },
    { op: 'setAttributes', id: 4, attrs: { type: 'masonry' } },
    { op: 'setAttributes', id: 4, attrs: { crossAxisGap: 4, mainAxisGap: 4 } },
    { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 5, value: 'width:100%;height:80px;background:#FF0000;' },
    { op: 'createElement', id: 6, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 6, value: 'width:100%;height:120px;background:#00FF00;' },
    { op: 'createElement', id: 7, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 7, value: 'width:100%;height:80px;background:#0000FF;' },
    { op: 'createElement', id: 8, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 8, value: 'width:100%;height:80px;background:#FFFF00;' },
    { op: 'createElement', id: 9, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 9, value: 'width:100%;height:120px;background:#00FFFF;' },
    { op: 'createElement', id: 10, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: 10, value: 'width:100%;height:80px;background:#FF00FF;' },
    { op: 'appendChild', id: 4, child: 5 },
    { op: 'appendChild', id: 4, child: 6 },
    { op: 'appendChild', id: 4, child: 7 },
    { op: 'appendChild', id: 4, child: 8 },
    { op: 'appendChild', id: 4, child: 9 },
    { op: 'appendChild', id: 4, child: 10 },
    { op: 'appendChild', id: 3, child: 4 },
    { op: 'appendChild', id: 2, child: 3 },
    { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
    { op: 'appendChild', id: 1, child: 2 },
    { op: 'attach', root: 1 },
    { op: 'startRender' },
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-grid-scroll-'));
  const fixture = path.join(dir, 'grid-scroll.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({ ops }));
  try {
    const { result, logs } = runOps(fixture, {
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /gridview #4: 6 items/);

    const rectOf = (id) => {
      const match = new RegExp(
        `\\[ops-debug\\] rect #${id} left=([-\\d.]+) top=([-\\d.]+) ` +
        'w=([-\\d.]+) h=([-\\d.]+)').exec(logs);
      assert.ok(match, `no rect published for node #${id}:\n${logs}`);
      return match.slice(1).map(Number);
    };
    // Content-space packing: red (0,0), green (162,0), blue (0,84), yellow
    // (162,124), cyan (0,168), magenta (162,208).  Subtracting scrollTop=100
    // yields the viewport positions below.
    assert.deepEqual(rectOf(5), [0, -100, 158, 80]);
    assert.deepEqual(rectOf(6), [162, -100, 158, 120]);
    assert.deepEqual(rectOf(7), [0, -16, 158, 80]);
    assert.deepEqual(rectOf(8), [162, 24, 158, 80]);
    assert.deepEqual(rectOf(9), [0, 68, 158, 120]);
    assert.deepEqual(rectOf(10), [162, 108, 158, 80]);
    // The scroll viewport itself stays anchored to the page.
    assert.deepEqual(rectOf(3), [0, 0, 320, 200]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: mouse drag scrolls the scroll view', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const script = path.join(os.tmpdir(), `skyline-mouse-drag-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1400 madd 160 100 1',
    '1500 mdown 160 100 1',
    '1600 mmove 160 90 1',
    '1700 mmove 160 80 1',
    '1800 mmove 160 70 1',
    '1900 mmove 160 60 1',
    '2000 mup 160 60 1',
    '',
  ].join('\n'));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    // SkylineScrollBehavior includes PointerDeviceKind.mouse in dragDevices.
    assert.match(logs, /scroll: top=[1-9]\d*(\.\d+)? /);
    assert.ok(pixels, 'no frame was captured');
    let firstGreen = height;
    for (let y = 0; y < height; y++) {
      const [r, g] = pixelAt(pixels, 160, y);
      if (g > 200 && r < 100) {
        firstGreen = y;
        break;
      }
    }
    assert.ok(firstGreen > 60 && firstGreen < 100,
      `expected mouse drag to scroll, red/green boundary at y=${firstGreen}`);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: list-builder mounts items on demand', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-builder.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    debugLogs: true,
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // 100 declared children, but only the visible window is materialized.
  const indexes = [...logs.matchAll(/builder mount: #4 index=(\d+)/g)]
    .map((match) => Number(match[1]));
  const unique = new Set(indexes);
  assert.ok(unique.size >= 3, `expected several mounts, got ${unique.size}`);
  assert.ok(unique.size < 20,
    `builder should not mount all captured items, got ${unique.size}`);
  for (const index of unique) {
    assert.ok(index < 20, `unexpected mounted index ${index}`);
  }

  // Alternate red/green 40px rows, white below the 200px viewport.
  assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 50), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 10, 210), [255, 255, 255, 255]);
});

test('appservice ops: fixed nodes stay pinned inside a scrollview', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-fixed-scroll.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    debugLogs: true,
    timeoutMs: 2800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /appservice setScrollTop: id=3 value=100/);
  assert.ok(pixels, 'no frame was captured');
  // Content scrolled by 100px; the fixed pin remains at the viewport corner.
  assert.deepEqual(pixelAt(pixels, 285, 210), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 285, 110), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 100, 10), [255, 0, 0, 255]);
});

test('appservice ops: builder updates use fine-grained callbacks', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-builder-update.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    debugLogs: true,
    timeoutMs: 3600,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /ListBuilderUpdateChildCount: #4 childCount=30/);
  assert.match(logs, /ListBuilderItemUpdateChildren: #4 index=0/);
  assert.match(logs, /ListBuilderUpdateChildHeight: #4 height=50/);
  assert.match(logs, /ListBuilderUpdateChildren: #4 \(5 item\(s\), 2 updated\)/);
  assert.ok(!/appservice flush:/.test(logs),
    `builder updates should not rebuild the attached page:\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  // item 0 (y=0..49) is magenta, item 1 (y=50..99) keeps its green class,
  // item 2 (y=100..149) is yellow.
  assert.deepEqual(pixelAt(pixels, 160, 10), [255, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 60), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 110), [255, 255, 0, 255]);
});

test('appservice ops: position sticky pins the header while scrolling', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-sticky.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // scrollTop=200: the 40px header stays pinned at the viewport top while the
  // 400px body scrolls underneath.
  assert.match(logs, /scrollview #3 .*type=3 .*sticky=1/);
  assert.match(logs, /sticky header 0x[0-9a-f]+ onTop=1/);
  assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255], 'pinned header');
  assert.deepEqual(pixelAt(pixels, 10, 35), [255, 0, 0, 255], 'header bottom');
  assert.deepEqual(pixelAt(pixels, 10, 45), [0, 170, 0, 255], 'body below header');
  assert.deepEqual(pixelAt(pixels, 160, 100), [0, 170, 0, 255], 'scrolled body');
  assert.deepEqual(pixelAt(pixels, 160, 130), [255, 255, 255, 255], 'outside scroll view');
});

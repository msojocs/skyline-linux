'use strict';

// Appservice op replay: Builtin components: swiper, picker, input, observers, semantics, native views and share-element.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {
  checkOpsBuild,
  countColor,
  opsSkip,
  pixelAt,
  repoRoot,
  runOps,
} = require('./helpers');

checkOpsBuild('appservice ops (widgets)');

test('appservice ops: swiper height:auto overrides the builtin 150px base rule', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors `.product-list-wrapper { flex: 1; height: auto; background:
  // white }` inside a flex row.  The base stylesheet's `swiper{height:150px}`
  // must lose to the class's `auto`, the swiper must stretch to the row
  // height, and its CSS background must paint.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-swiper-auto-height.ops.json');
  const { result, logs, pixels } = runOps(fixture);

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // The 60px cell paints blue at the top of the stretched swiper; the rest of
  // the swiper box is its red background (not the green parent row).
  assert.deepEqual(pixelAt(pixels, 160, 10), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 100), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 239), [255, 0, 0, 255]);
});

test('appservice ops: swiper callbacks and programmatic paging', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-swiper-'));
  const ops = path.join(dir, 'swiper.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '.page{width:320px;height:240px;background:#FFFFFF;}' +
          '.sw{position:absolute;left:10px;top:10px;width:200px;height:100px;}' +
          '.item{width:200px;height:100px;}' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'addClass', id: 2, prefix: 'page', name: 1 },
      { op: 'createElement', id: 3, tag: 'swiper', name: 'swiper' },
      { op: 'addClass', id: 3, prefix: 'sw', name: 1 },
      { op: 'setAttributes', id: 3,
        attrs: { current: 0, duration: 0, interval: 5000 } },
      { op: 'createElement', id: 4, tag: 'swiper-item', name: 'swiper-item' },
      { op: 'addClass', id: 4, prefix: 'item', name: 1 },
      { op: 'setStyle', id: 4, value: 'background:#FF0000;' },
      { op: 'createElement', id: 5, tag: 'swiper-item', name: 'swiper-item' },
      { op: 'addClass', id: 5, prefix: 'item', name: 1 },
      { op: 'setStyle', id: 5, value: 'background:#00FF00;' },
      { op: 'createElement', id: 6, tag: 'swiper-item', name: 'swiper-item' },
      { op: 'addClass', id: 6, prefix: 'item', name: 1 },
      { op: 'setStyle', id: 6, value: 'background:#0000FF;' },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 3, child: 5 },
      { op: 'appendChild', id: 3, child: 6 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 800, ops: [
        { op: 'swiperTo', id: 3, page: 1, animated: false },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs,
      /swiper callbacks installed #3 \(onChange=1 onScroll=1\)/);
    assert.match(logs, /swiper scroll: id=3 offset=0 state=0 current=1/);
    assert.match(logs, /swiper change: id=3 page=1 reason=2/);
    assert.match(logs, /swiperTo #3 page=1 animated=0/);
    assert.ok(pixels, 'no frame was captured');
    // The swiper jumped to the second page (green).
    assert.deepEqual(pixelAt(pixels, 100, 50), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 100, 20), [0, 255, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: swiper indicator dots default to bottom center', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // `<swiper indicator-dots>` with no position override must place the dots at
  // the bottom centre (the SDK's default when the alignment argument is NaN).
  // Passing -1/-1 used to be read as Alignment(-1, -1) = top-left.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-swiper-dots-'));
  const ops = path.join(dir, 'swiper-dots.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '.page{width:320px;height:240px;background:#FFFFFF;}' +
          '.sw{position:absolute;left:10px;top:10px;width:200px;height:100px;}' +
          '.item{width:200px;height:100px;}' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'addClass', id: 2, prefix: 'page', name: 1 },
      { op: 'createElement', id: 3, tag: 'swiper', name: 'swiper' },
      { op: 'addClass', id: 3, prefix: 'sw', name: 1 },
      { op: 'setAttributes', id: 3,
        attrs: { current: 0, duration: 0, indicatorDots: true } },
      { op: 'createElement', id: 4, tag: 'swiper-item', name: 'swiper-item' },
      { op: 'addClass', id: 4, prefix: 'item', name: 1 },
      { op: 'setStyle', id: 4, value: 'background:#FF0000;' },
      { op: 'createElement', id: 5, tag: 'swiper-item', name: 'swiper-item' },
      { op: 'addClass', id: 5, prefix: 'item', name: 1 },
      { op: 'setStyle', id: 5, value: 'background:#00FF00;' },
      { op: 'createElement', id: 6, tag: 'swiper-item', name: 'swiper-item' },
      { op: 'addClass', id: 6, prefix: 'item', name: 1 },
      { op: 'setStyle', id: 6, value: 'background:#0000FF;' },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 3, child: 5 },
      { op: 'appendChild', id: 3, child: 6 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 3000,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // Swiper spans x 10..210, y 10..110.  The three dots must sit in the
    // bottom band, centred, with the active dot white and the inactive dots a
    // dimmed black over the red item.  The old -1/-1 alignment rendered them
    // in the top-left corner.
    let whiteBottom = 0;
    let whiteTop = 0;
    let dimBottom = 0;
    for (let y = 10; y < 110; y++) {
      for (let x = 10; x < 210; x++) {
        const [r, g, b] = pixelAt(pixels, x, y);
        const white = r > 220 && g > 220 && b > 220;
        if (white && y >= 90) whiteBottom++;
        if (white && y < 40) whiteTop++;
        if (y >= 90 && r > 120 && r < 220 && g < 80 && b < 80) dimBottom++;
      }
    }
    assert.ok(whiteBottom > 10,
      `active dot missing at the bottom centre (white=${whiteBottom})`);
    assert.equal(whiteTop, 0,
      `unexpected indicator at the swiper top (white=${whiteTop})`);
    assert.ok(dimBottom > 20,
      `inactive dots missing at the bottom centre (dim=${dimBottom})`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: picker-view-column events and programmatic value', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-picker-'));
  const ops = path.join(dir, 'picker.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'picker-view', name: 'picker-view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:10px;top:10px;width:200px;height:150px;' +
          'display:flex;flex-direction:row;' },
      { op: 'createElement', id: 4, tag: 'picker-view-column',
        name: 'picker-view-column' },
      { op: 'setStyle', id: 4, value: 'width:100px;height:150px;' },
      { op: 'setAttributes', id: 4,
        attrs: { value: 0, indicatorHeight: 50 } },
      { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 5,
        value: 'width:100px;height:50px;background:#FF0000;' },
      { op: 'createElement', id: 6, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 6,
        value: 'width:100px;height:50px;background:#00FF00;' },
      { op: 'createElement', id: 7, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 7,
        value: 'width:100px;height:50px;background:#0000FF;' },
      { op: 'appendChild', id: 4, child: 5 },
      { op: 'appendChild', id: 4, child: 6 },
      { op: 'appendChild', id: 4, child: 7 },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 800, ops: [{ op: 'pickerTo', id: 4, value: 2 }] },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs,
      /picker callbacks installed #4 \(onChange=1 onStart=1 onEnd=1\)/);
    assert.match(logs, /picker change: id=4 index=2 reason=1/);
    assert.match(logs, /picker start: id=4/);
    assert.match(logs, /picker end: id=4/);
    assert.match(logs, /pickerTo #4 current=2/);
    assert.ok(pixels, 'no frame was captured');
    // The third row (blue) is the selected centre item.
    assert.ok(countColor(pixels, (r, g, b) => b > 200 && r < 80 && g < 80) > 0,
      'expected the blue picker row to be visible after pickerTo(2)');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: programmatic input focus and blur', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-input-focus-'));
  const ops = path.join(dir, 'input.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'text', name: 'input' },
      { op: 'setText', id: 3, text: 'hello' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:10px;top:10px;width:200px;height:40px;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 600, ops: [
        { op: 'focusInput', id: 3, cursor: 5, selectionStart: 5,
          selectionEnd: 5 },
      ] },
      { afterMs: 1200, ops: [{ op: 'blurInput', id: 3 }] },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, { timeoutMs: 2500, frames: 0 });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    // InputFocusManually/BlurManually run against the real NewInput handle.
    assert.match(logs, /InputFocusManually: id=3/);
    assert.match(logs, /focus input: id=3/);
    assert.match(logs, /InputBlurManually: id=3/);
    assert.match(logs, /blur input: id=3/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: intersection observer, selection range and back interception', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-observer-'));
  const ops = path.join(dir, 'observer.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:10px;top:10px;width:300px;' +
          'height:100px;background:#EEEEEE;' },
      { op: 'createElement', id: 4, tag: 'text', name: '' },
      { op: 'setText', id: 4, text: 'hello world' },
      { op: 'setAttributes', id: 4, attrs: { userSelect: true } },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'createElement', id: 5, tag: 'selection', name: 'selection' },
      { op: 'setAttributes', id: 5, attrs: {
        disableContextMenu: true, selectionColor: '#3399FF80',
      } },
      { op: 'appendChild', id: 5, child: 3 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 700, ops: [
        { op: 'addIntersectionObserver', id: 3, uniqueId: 7,
          thresholds: [0, 0.5, 1] },
        { op: 'intersect', id: 3, uniqueId: 7, ratio: 0.6,
          intersecting: true },
        { op: 'selectionRange', selectionId: 5, startId: 4, startOffset: 0,
          endId: 4, endOffset: 5 },
        { op: 'navigateBackIntercept', pageId: 1, enabled: true },
        { op: 'triggerNavigateBackIntercept', pageId: 1 },
      ] },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 3000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0, `skyline host failed (status ${result.status}):\n${logs}`);
    assert.doesNotMatch(logs, /Incorrect use of ParentDataWidget/);
    assert.match(logs,
      /intersection add: node=#3 id=7 root=#0 thresholds=3/);
    assert.match(logs,
      /intersection trigger: node=#3 id=7 ratio=0\.6 intersecting=1/);
    assert.match(logs, /selection callbacks installed #5/);
    assert.match(logs, /selection setRange: selection=#5 start=#4:0 end=#4:5/);
    assert.match(logs, /selection change: id=5 .* text="hello"/);
    assert.match(logs, /navigate back interception: page=1 enabled=1/);
    assert.match(logs, /OnNavigateBackIntercept: page=1 enabled=1/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: accessibility semantics, dumps and automation APIs', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-automation-'));
  const ops = path.join(dir, 'automation.ops.json');
  const snapshotDir = path.join(dir, 'snapshots');
  fs.mkdirSync(snapshotDir);
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:10px;top:10px;width:100px;' +
          'height:100px;background:#00AA00;' },
      { op: 'setAttributes', id: 3, attrs: {
        'aria-label': 'album cover', 'aria-role': 'img',
      } },
      { op: 'createElement', id: 4, tag: 'text', name: '' },
      { op: 'setText', id: 4, text: 'Play' },
      { op: 'setStyle', id: 4,
        value: 'position:absolute;left:140px;top:10px;font-size:20px;' },
      { op: 'setAttributes', id: 4, attrs: {
        'aria-label': 'play button', 'aria-role': 'button',
      } },
      { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 5,
        value: 'position:absolute;left:10px;top:140px;width:80px;' +
          'height:40px;background:#FF0000;' },
      { op: 'setAttributes', id: 5, attrs: { 'aria-hidden': 'true' } },
      // `<snapshot>` exercises NewSnapshot + SnapshotTakeSnapshot + SceneToImage.
      { op: 'createElement', id: 6, tag: 'snapshot', name: 'snapshot' },
      { op: 'setStyle', id: 6,
        value: 'position:absolute;left:200px;top:10px;width:60px;' +
          'height:60px;' },
      { op: 'createElement', id: 7, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 7,
        value: 'width:60px;height:60px;background:#0000FF;' },
      { op: 'appendChild', id: 6, child: 7 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'appendChild', id: 2, child: 4 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'appendChild', id: 2, child: 6 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 500, ops: [
        { op: 'accessibility', enabled: true },
      ] },
      { afterMs: 1500, ops: [
        // Semantics* updates the materialized SkylineSemantics in place.
        { op: 'setAttribute', id: 3, name: 'aria-label',
          value: 'updated cover' },
        { op: 'dumpElementTree' },
        { op: 'dumpRenderTree' },
        { op: 'collectGarbage' },
        { op: 'sendMessageToRunner', message: 'hello runner' },
        { op: 'userGesture', active: true },
        { op: 'userGesture', active: false },
        { op: 'takeAppSnapshot', requestId: 11, scale: 1, compressed: true },
        { op: 'snapshotNode', id: 3, format: 4, requestId: 12 },
        { op: 'snapshotNode', id: 6, format: 4, requestId: 13 },
      ] },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 6500,
      frames: 0,
      env: {
        SKYLINE_APP_SERVICE_DEBUG: '1',
        SKYLINE_SNAPSHOT_DIR: snapshotDir,
      },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.doesNotMatch(logs, /Incorrect use of ParentDataWidget/);

    // Accessibility: the runtime builds SkylineSemantics through NewSemantics,
    // the host applies the feature/semantics flags, and the framework emits a
    // semantics tree with the aria-label.
    assert.match(logs, /semantics #3 label="album cover" role="img" hidden=0/);
    assert.match(logs,
      /semantics #4 label="play button" role="button" hidden=0/);
    assert.match(logs, /semantics #5 label="" role="" hidden=1/);
    assert.match(logs, /UpdateAccessibility: enabled=1/);
    assert.match(logs, /AccessibilityBoldTextEnabled: [01]/);
    assert.match(logs, /semantics update #3 label="updated cover" role="img"/);
    assert.match(logs, /semantics enabled: 1/);
    assert.match(logs, /semantics node: id=\d+ label="updated cover"/);

    // Automation.
    assert.match(logs, /DumpElementTree requested \(callback=yes\)/);
    assert.match(logs, /\[skyline-runtime:I\] : dumpElementTree/);
    assert.match(logs, /YGNodeRenderObject/);
    assert.match(logs, /DumpRenderTree requested \(callback=yes\)/);
    assert.match(logs, /CollectGarbage requested \(callback=yes\)/);
    assert.match(logs, /SendMessageToRunner: "hello runner" \(callback=yes\)/);
    assert.match(logs, /user gesture: start \(callback=yes\)/);
    assert.match(logs, /user gesture: stop \(callback=yes\)/);
    assert.match(logs,
      /app snapshot: request=11 320x240 format=png bytes=\d+ ok=1/);
    assert.match(logs,
      /node snapshot: request=12 format=png bytes=\d+ ok=1/);
    // `<snapshot>` path: NewSnapshot → SnapshotTakeSnapshot → SceneToImage.
    assert.match(logs,
      /snapshot widget request: node=#6 format=4 request=13 \(callback=yes\)/);
    assert.match(logs, /snapshot widget: node=#6 accepted=1/);
    assert.match(logs, /SceneToImage: 60x60 format=4 bytes=\d+/);
    assert.match(logs,
      /node snapshot: request=13 format=png bytes=\d+ ok=1/);

    // Snapshot payloads are also written for DevTools automation.
    const files = fs.readdirSync(snapshotDir);
    assert.ok(files.includes('snapshot-app-11.png'),
      `missing app snapshot (${files.join(',')})`);
    assert.ok(files.includes('snapshot-node-12.png'),
      `missing node snapshot (${files.join(',')})`);
    assert.ok(files.includes('snapshot-node-13.png'),
      `missing widget snapshot (${files.join(',')})`);
    const appPng = fs.readFileSync(path.join(snapshotDir, 'snapshot-app-11.png'));
    assert.equal(appPng.subarray(1, 4).toString('ascii'), 'PNG');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: native view forwards pointer events', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-native-view.ops.json');
  const script = path.join(os.tmpdir(), `skyline-native-view-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1200 add 60 60 1',
    '1300 down 60 60 1',
    '1400 move 70 70 1',
    '1500 up 70 70 1',
    '',
  ].join('\n'));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');

    // The `<texture>` node materialises as a native-view slot and keeps its
    // JS-visible viewId for pointer routing.
    assert.match(logs, /native view #4: viewId=77 type=canvas/);
    assert.match(logs,
      /HandleNativeViewPointerEvent: viewId=77 node=4 type=down .*pos=\(40,40\)/);
    assert.match(logs,
      /HandleNativeViewPointerEvent: viewId=77 node=4 type=move .*pos=\(50,50\).*delta=\(10,10\)/);
    assert.match(logs,
      /HandleNativeViewPointerEvent: viewId=77 node=4 type=up .*pos=\(50,50\)/);

    // The red page shows the opaque native-view placeholder at 20,20 100x80.
    assert.deepEqual(pixelAt(pixels, 10, 10), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 60), [0, 0, 0, 255]);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Missing file is fine.
    }
  }
});

test('appservice ops: releasing a native view disposes its texture handle', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-native-view-release.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 2500,
    frames: 0,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  assert.match(logs, /native view #4: viewId=77 type=canvas/);
  assert.match(logs, /native view dispose: viewId=77/);

  // The update removes the texture; the red page fills its slot again.
  assert.deepEqual(pixelAt(pixels, 60, 60), [255, 0, 0, 255]);
});

test('appservice ops: swiper current attribute animates to the next page', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-swiper.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 2200,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // The update mirrors the appservice's `current` attribute after a change;
  // the swiper must animate to the blue page instead of going blank.
  assert.deepEqual(pixelAt(pixels, 160, 60), [0, 0, 255, 255],
    'second swiper page is visible');
  assert.deepEqual(pixelAt(pixels, 20, 20), [0, 0, 255, 255],
    'first swiper page moved offscreen');
});

test('appservice ops: share-element builds a SkylineHero with its WXML props', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // card_transition's list cards forward their resolved props through
  // `setAttributes`: the builtin component sets `tag`/`onFrame` and the
  // bootstrap backfill resends `transitionOnGesture`/`rectTweenType`/
  // `shuttleOn*`.  The runtime must build a SkylineHero (Flutter's Hero
  // machinery drives the shared-element flight) and register the
  // `onFrame-<viewTag>` worklet hash for the flight frames.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-share-element-'));
  const ops = path.join(dir, 'share-element.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'share-element', name: 'share-element' },
      { op: 'setStyle', id: 3, value: 'width:160px;height:120px;' },
      { op: 'createElement', id: 4, tag: 'view', name: 'view' },
      { op: 'setStyle', id: 4, value: 'width:100%;height:100%;background:#FF0000;' },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'setAttributes', id: 3, attrs: {
        tag: 'se-key0',
        onFrame: 'handleFrame',
        transitionOnGesture: true,
        rectTweenType: 'cubic-bezier(0.4, 0.0, 0.2, 1.0)',
        shuttleOnPush: 'from',
        shuttleOnPop: 'to',
      } },
      { op: 'attach', root: 2 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 3000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.doesNotMatch(logs, /Unhandled exception/);
    assert.match(logs,
      /hero #3 tag=se-key0 gesture=1 onFrame=onFrame-3 tween=cubic-bezier\(0\.4, 0\.0, 0\.2, 1\.0\) push=from pop=to/,
      `expected a SkylineHero for the share-element:\n${logs}`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

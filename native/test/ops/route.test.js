'use strict';

// Appservice op replay: Routes, the page stack and app-bar overlays.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const {
  checkOpsBuild,
  countColor,
  height,
  legacyBundle,
  opsSkip,
  pixelAt,
  recordRealOps,
  repoRoot,
  runOps,
  width,
} = require('./helpers');

checkOpsBuild('appservice ops (route)');

test('appservice ops: app-bar context installs an overlay, not a page route', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors `app.json#appBar`: the SDK creates a second render context and
  // attaches it through `attachAppBar`.  The runtime must hand the tree to
  // Dart `CreateAppBar` (window overlay) instead of materialising another
  // route, otherwise the app bar paints on top of the page as a full page.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-appbar.ops.json');
  const { result, logs, pixels } = runOps(fixture, { dpr: 2 });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  assert.match(logs, /app bar -> CreateAppBar \(root=10, 320x240\)/);
  assert.equal((logs.match(/appservice page \d+ -> CreatePageRoute/g) || []).length,
    1, 'the app-bar context must not create a second page route');

  // Page content (red 100x100 block) stays visible; the app-bar overlay draws
  // its bottom bar (green, 120x60 CSS) above it.
  const pixelAt2 = (x, y) => {
    const offset = (y * width * 2 + x) * 4;
    return [...pixels.subarray(offset, offset + 4)];
  };
  assert.deepEqual(pixelAt2(20, 20), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt2(240, 20), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt2(20, 400), [0, 170, 0, 255]);
  assert.deepEqual(pixelAt2(20, 200), [255, 255, 255, 255]);
});

test('appservice ops: route stack pushes and pops pages', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-route.ops.json');
  const { result, logs, pixels, details } = runOps(fixture, {
    timeoutMs: 2200,
    frames: 0,
  });
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  assert.ok(details.presentCount >= 3,
    `expected push/pop frames, presentCount=${details.presentCount}`);
  assert.match(logs, /appservice page 1 -> CreatePageRoute/);
  assert.match(logs, /appservice page 2 -> CreatePageRoute \(openType=4/);
  assert.match(logs, /didPush pageId: 1/);
  assert.match(logs, /NavigateBack: delta=1 animated=1/);
  // Page B (red) is pushed, then popped back to page A (blue).
  assert.deepEqual(pixelAt(pixels, 160, 120), [0, 0, 255, 255]);
});

test('appservice ops: recorded SPA navigation replays a route stack', {
  skip: opsSkip,
  timeout: 90000,
}, () => {
  const bundle = legacyBundle();
  if (bundle === null) return;  // official renderer bundle not installed
  const recorded = recordRealOps({ bundle, tapText: 'Jump To PageB' });
  if (recorded === null) return;
  try {
    assert.ok((recorded.payload.updates || []).length >= 1,
      'tap should emit a page-B operation batch');
    const { result, logs, pixels } = runOps(recorded.path, {
      timeoutMs: 3000,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /appservice page 2 -> CreatePageRoute \(openType=4/);
    assert.match(logs, /didPush pageId: 1/);
    assert.ok(pixels, 'no frame was captured');
    // Page B renders "I am PageB" (the buttons sit below its 200px spacer).
    const dark = countColor(pixels,
      (r, g, b) => r < 100 && g < 100 && b < 100);
    assert.ok(dark > 50, `expected PageB text, found ${dark} dark pixels`);
  } finally {
    try {
      fs.unlinkSync(recorded.path);
    } catch {
      // Missing file is fine.
    }
  }
});

test('appservice ops: custom route attach configures CreatePageRoute', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-custom-route-'));
  const ops = path.join(dir, 'custom-route.ops.json');
  // Push a custom-route page, navigate back, then push another one.  The SDK
  // (SkylineJS `attach()`) emits `attach` first and `attachCustomRoute` right
  // after it in the same flush batch; a runtime that builds the route on the
  // attach op loses the config and leaves it pending for the next push, which
  // then reuses the previous page's host and renders nothing.
  const customPage = (hostId, pageId, viewId, color, animTag) => [
    // Custom routes attach a `host` wrapper that contains the page root.
    { op: 'createElement', id: hostId, tag: 'empty', name: 'host' },
    { op: 'createElement', id: pageId, tag: 'empty', name: 'page' },
    // card_transition's detail.wxss makes the page transparent so the covered
    // list route shows while the page translates during a back gesture.
    { op: 'setStyle', id: pageId, value: 'background-color: transparent;' },
    { op: 'createElement', id: viewId, tag: 'empty', name: 'view' },
    { op: 'setStyle', id: viewId,
      value: `width:320px;height:240px;background:${color};` },
    { op: 'appendChild', id: pageId, child: viewId },
    { op: 'appendChild', id: hostId, child: pageId },
    { op: 'attach', root: pageId, host: hostId },
    // rConfig order from assets/app.js attachCustomRoute().
    { op: 'attachCustomRoute', args: [
      true, true, true, 300, 300, 'rgba(0, 0, 0, 0.3)', false, '', true, true,
      `onAnimationChange-${animTag}`, `onSecondaryAnimationChange-${animTag}`,
    ] },
    { op: 'startRender' },
  ];
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#0000FF;' },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'notifyNavigateTo', args: [0, 2] },
        ...customPage(20, 21, 22, '#FF0000', 20),
      ] },
      { afterMs: 900, ops: [
        { op: 'notifyNavigateBack', args: [0, 2, 1, true] },
      ] },
      { afterMs: 1500, ops: [
        { op: 'notifyNavigateTo', args: [0, 3] },
        ...customPage(40, 41, 42, '#00AA00', 40),
      ] },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 3500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.doesNotMatch(logs, /Incorrect use of ParentDataWidget/);
    assert.doesNotMatch(logs, /Unhandled exception/);
    assert.match(logs, /attachCustomRoute custom=1 .*anim=onAnimationChange-20/);
    assert.match(logs, /attachCustomRoute custom=1 .*anim=onAnimationChange-40/);
    // The route builder returns a CSS color string; it must reach Dart as
    // 30%-alpha black, not be dropped (the covered page shows through it).
    assert.match(logs, /attachCustomRoute .*barrier=4c000000/);
    // Both pushes are custom routes; neither may fall back to a plain route
    // (that fallback also means the config leaked into the next attach).
    const pushes = [...logs.matchAll(
      /appservice page \d+ -> CreatePageRoute \(openType=4, 320x240, custom=(\d), bg=([0-9a-f]+)\)/g,
    )].map((match) => [match[1], match[2]]);
    assert.deepEqual(pushes, [['1', '00000000'], ['1', '00000000']],
      `expected two custom route pushes with a transparent page background:\n` +
      logs);
    assert.match(logs,
      /appservice page 1 -> CreatePageRoute \(openType=0, 320x240, custom=0, bg=ff0000ff\)/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: rebuilding the top page keeps covered route images', {
  skip: opsSkip,
  timeout: 60000,
}, async () => {
  // The node table is shared with the route kept alive under the current one.
  // A coarse rebuild of the top page must not reset the covered page's
  // materialized handles: its retained widgets (and direct-render image
  // callbacks) keep painting while a non-opaque route is revealed.
  //
  // The image arrives after the rebuild (network fetch), so the late-load
  // notification has to reach the covered route's widget.  The home page is
  // attached with its `page` element as the attach root (like the DevTools
  // appservice), which makes the wrapper body a non-covered ancestor: a reset
  // that recurses into the body used to wipe the covered page's image
  // registration, leaving the image blank forever.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-covered-image-'));
  const redPng =
    'iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAFUlEQVR4nGP8z8Dw' +
    'nwEJMDGgAcICAIPRAgYCkO9YAAAAAElFTkSuQmCC';
  const server = spawn(process.execPath, ['-e', `
    const http = require('http');
    const bytes = Buffer.from('${redPng}', 'base64');
    const server = http.createServer((req, res) => {
      setTimeout(() => {
        res.writeHead(200, {
          'Content-Type': 'image/png',
          'Content-Length': String(bytes.length),
        });
        res.end(bytes);
      }, 700);
    });
    server.listen(0, '127.0.0.1', () => console.log(server.address().port));
  `], { stdio: ['ignore', 'pipe', 'inherit'] });
  let port = 0;
  try {
    port = await new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('image server did not start')), 5000);
      server.stdout.once('data', (data) => {
        clearTimeout(timer);
        resolve(Number(String(data).trim()));
      });
    });
    const imageUrl = `http://127.0.0.1:${port}/red.png`;
    const fixture = path.join(dir, 'covered-image.ops.json');
    fs.writeFileSync(fixture, JSON.stringify({
      ops: [
        { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
        { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
        { op: 'setStyle', id: 2,
          value: 'width: 320px; height: 240px; background: #FFFFFF;' },
        { op: 'createElement', id: 3, tag: 'image', name: 'image' },
        { op: 'setStyle', id: 3,
          value: 'position: absolute; left: 10px; top: 10px; ' +
            'width: 80px; height: 80px;' },
        { op: 'setAttributes', id: 3, attrs: { src: imageUrl } },
        { op: 'appendChild', id: 2, child: 3 },
        { op: 'appendChild', id: 1, child: 2 },
        // The attach root is the page element, not its wrapper.
        { op: 'attach', root: 2 },
        { op: 'startRender' },
      ],
      updates: [
        { afterMs: 150, ops: [
          { op: 'notifyNavigateTo', args: [0, 2] },
          { op: 'createElement', id: 20, tag: 'empty', name: 'host' },
          { op: 'createElement', id: 21, tag: 'empty', name: 'page' },
          { op: 'setStyle', id: 21,
            value: 'width: 320px; height: 240px; ' +
              'background-color: transparent;' },
          { op: 'createElement', id: 22, tag: 'empty', name: 'view' },
          { op: 'setStyle', id: 22,
            value: 'position: absolute; left: 160px; top: 0; ' +
              'width: 160px; height: 240px; background: #00FF00;' },
          { op: 'appendChild', id: 21, child: 22 },
          { op: 'appendChild', id: 20, child: 21 },
          { op: 'attach', root: 21, host: 20 },
          { op: 'attachCustomRoute', args: [
            true, false, true, 300, 300, 'rgba(0, 0, 0, 0.3)', false, '',
            false, false,
            'onAnimationChange-20', 'onSecondaryAnimationChange-20',
          ] },
          { op: 'startRender' },
        ] },
        // Structural change on the top page -> coarse rebuild of the top route
        // while the home image is still loading.
        { afterMs: 400, ops: [
          { op: 'createElement', id: 23, tag: 'empty', name: 'view' },
          { op: 'setStyle', id: 23,
            value: 'position: absolute; left: 0px; top: 0px; ' +
              'width: 10px; height: 10px; background: #0000FF;' },
          { op: 'appendChild', id: 21, child: 23 },
          { op: 'startRender' },
        ] },
      ],
    }));
    const { result, logs, pixels } = runOps(fixture, {
      timeoutMs: 2200,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // The non-opaque route leaves the home page visible on the left, dimmed
    // by its 30%-black barrier.  The late image must still be there.
    assert.deepEqual(pixelAt(pixels, 40, 40), [179, 0, 0, 255],
      'covered route image was cleared by the top page rebuild');
    // The top page's own content still paints on the right.
    assert.deepEqual(pixelAt(pixels, 250, 120), [0, 255, 0, 255]);
  } finally {
    server.kill();
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: left-edge swipe pops a plain navigateTo route', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-edge-back-'));
  const ops = path.join(dir, 'edge-back.ops.json');
  const script = path.join(dir, 'edge-back.pointer.txt');
  // Home page (blue) pushes a red page via notifyNavigateTo.  No
  // attachCustomRoute: this is the Flutter DismissiblePage edge gesture.
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#0000FF;' },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'notifyNavigateTo', args: [0, 2] },
        { op: 'createElement', id: 20, tag: 'empty', name: 'page' },
        { op: 'setStyle', id: 20,
          value: 'width:320px;height:240px;background:#FF0000;' },
        { op: 'attach', root: 20 },
        { op: 'startRender' },
      ] },
    ],
  }));
  // Pass the 50% dismiss threshold (or fling faster than one screen width
  // per second) so the Cupertino back controller commits the pop.
  fs.writeFileSync(script, [
    '1300 add 5 120 1',
    '1400 down 5 120 1',
    '1450 move 60 120 1',
    '1500 move 120 120 1',
    '1550 move 180 120 1',
    '1600 move 240 120 1',
    '1700 up 260 120 1',
    '',
  ].join('\n'));
  try {
    const { result, logs, pixels } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /appservice page 2 -> CreatePageRoute \(openType=4/);
    assert.match(logs, /NotifyNavigateBackByGesture: .* popped=1/);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 160, 120), [0, 0, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

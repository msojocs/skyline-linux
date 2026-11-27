'use strict';

// Appservice op replay: Pointer and keyboard input, hit testing and Dart event channels.
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

checkOpsBuild('appservice ops (input)');

test('host pointer script reaches the Skyline hit test path', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const script = path.join(os.tmpdir(), `skyline-pointers-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1200 add 160 100 1',
    '1300 down 160 100 1',
    '1400 move 160 60 1',
    '1500 up 160 60 1',
    '',
  ].join('\n'));
  try {
    const { result, logs } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    // The embedder pointer event is hit-tested against the YGNode tree.
    assert.match(logs, /_handlePointerEvent: pointer: \d+, eventInfo: Instance of 'CachedEventInfo', node: YGNode/);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: fixed full-viewport overlay does not steal touches', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // A fixed overlay (e.g. root-portal content for an A-Z index) covers the
  // page, but the pointer is outside its only child.  The connector-backed
  // YGNode must not fall back to the whole-box RenderBox hit test, otherwise
  // the overlay swallows every touch and the scroll view behind it never
  // receives the drag.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-fixed-overlay-scroll.ops.json');
  const script = path.join(os.tmpdir(), `skyline-overlay-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1200 add 160 100 1',
    '1300 down 160 100 1',
    '1400 move 160 80 1',
    '1500 move 160 60 1',
    '1600 move 160 40 1',
    '1700 up 160 40 1',
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
    assert.ok(firstGreen < 100,
      `expected the drag to scroll the list under the overlay, green at y=${firstGreen}`);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: pointer-events:none overlay does not block touches', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // The fixed overlay covers the viewport and its blue child sits under the
  // drag origin.  With pointer-events:none the native YGNode is untouchable
  // (PointerEvents.none) and the mirror hit test skips its subtree, so the
  // scroll view below must receive the drag.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-pointer-events.ops.json');
  const script = path.join(os.tmpdir(), `skyline-pointer-events-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1200 add 10 100 1',
    '1300 down 10 100 1',
    '1400 move 10 60 1',
    '1500 move 10 40 1',
    '1600 move 10 20 1',
    '1700 up 10 20 1',
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
    assert.ok(firstGreen < 100,
      `expected pointer-events:none to let the drag reach the scroll view, ` +
      `green at y=${firstGreen}`);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: default overlay blocks touches (pointer-events control)', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Same geometry without pointer-events:none: the leaf overlay is touchable
  // and consumes the drag, so the scroll view below stays at the top.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-pointer-events-control.ops.json');
  const script = path.join(os.tmpdir(), `skyline-pointer-control-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1200 add 10 100 1',
    '1300 down 10 100 1',
    '1400 move 10 60 1',
    '1500 move 10 40 1',
    '1600 move 10 20 1',
    '1700 up 10 20 1',
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
    assert.doesNotMatch(logs, /scroll: top=[1-9]\d*(\.\d+)? /);
    assert.ok(pixels, 'no frame was captured');

    let firstGreen = height;
    for (let y = 0; y < height; y++) {
      const [r, g] = pixelAt(pixels, 160, y);
      if (g > 200 && r < 100) {
        firstGreen = y;
        break;
      }
    }
    assert.ok(firstGreen >= 100,
      `expected the default overlay to block the drag, green at y=${firstGreen}`);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: input and route channels receive Dart events', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const script = path.join(os.tmpdir(), `skyline-channels-${process.pid}.txt`);
  fs.writeFileSync(script, [
    '1400 add 160 100 1',
    '1500 down 160 100 1',
    '1600 move 160 90 1',
    '1700 up 160 90 1',
    '',
  ].join('\n'));
  try {
    const { result, logs } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    // Route transition channel: the initial CreatePageRoute completes.
    assert.match(logs, /HandlePageRouteDone: pageId=0/);
    // Touch channel: Dart forwards the pointer stream with the hit-test target
    // and pointer coordinates.
    assert.match(logs, /HandleTouchEvent: type=touchstart target=0x[0-9a-f]+ touches=1 first=\(id=1 client=\(160,100\)\)/);
    assert.match(logs, /HandleTouchEvent: type=touchmove .*first=\(id=1 client=\(160,90\)\)/);
    assert.match(logs, /HandleTouchEvent: type=touchend .*touches=0/);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: host key events drive the keyboard channel', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-key-'));
  const ops = path.join(dir, 'key.ops.json');
  const script = path.join(dir, 'key.script');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'text', name: 'input' },
      { op: 'setText', id: 3, text: 'A' },
      { op: 'setStyle', id: 3, value: 'font-size:48px;color:#000000;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
      { op: 'focusInput', id: 3 },
      { op: 'keyboardStatus', inputKey: 3, height: 180 },
    ],
  }));
  fs.writeFileSync(script, [
    '1500 down 4 98 b',
    '1600 up 4 98',
    '',
  ].join('\n'));
  try {
    const { result, logs, details } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
      args: ['--key-script', script],
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /focus input: id=3/);
    assert.match(logs, /UpdateKeyboardStatus: key=3 height=180/);
    assert.match(logs, /\[host\] key event sent: at=1500/);
    assert.match(logs, /DispatchSkylineKeyEvent: kind=0 logical=98 text=b/);
    assert.match(logs, /key input: id=3 value="Ab"/);
    assert.ok(details.presentCount >= 2, 'expected a repaint after the key event');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: editing keys move the cursor and replace selections', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-edit-'));
  const ops = path.join(dir, 'edit.ops.json');
  const script = path.join(dir, 'edit.script');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'text', name: 'input' },
      { op: 'setText', id: 3, text: 'Ab' },
      { op: 'setStyle', id: 3, value: 'font-size:48px;color:#000000;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
      { op: 'focusInput', id: 3, holdKeyboard: 1 },
    ],
  }));
  fs.writeFileSync(script, [
    '1500 down 0 37',
    '1600 down 0 88 X',
    '1700 down 0 36',
    '1800 down 0 46',
    '',
  ].join('\n'));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 2600,
      frames: 0,
      args: ['--key-script', script],
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /InputHoldKeyboard: id=3 hold=1/);
    assert.match(logs, /key input: id=3 value="AXb" cursor=2/);
    assert.match(logs, /key input: id=3 value="AXb" cursor=0/);
    assert.match(logs, /key input: id=3 value="Xb" cursor=0/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: composition replaces the selected range', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-ime-'));
  const ops = path.join(dir, 'ime.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'text', name: 'input' },
      { op: 'setText', id: 3, text: 'Ab' },
      { op: 'setStyle', id: 3, value: 'font-size:48px;color:#000000;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
      { op: 'focusInput', id: 3, holdKeyboard: 1 },
    ],
    updates: [
      { afterMs: 200, ops: [
        { op: 'setSelection', id: 3, start: 0, end: 1, cursor: 1 },
      ] },
      { afterMs: 300, ops: [
        { op: 'compositionStart', id: 3, text: 'n' },
      ] },
      { afterMs: 500, ops: [
        { op: 'compositionUpdate', id: 3, text: 'ni' },
      ] },
      { afterMs: 700, ops: [
        { op: 'compositionEnd', id: 3, text: '你' },
      ] },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      timeoutMs: 2200,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /setSelection: id=3 selection=\[0,1\] cursor=1/);
    assert.match(logs, /composition start: id=3 text="n"/);
    assert.match(logs, /composition update: id=3 text="ni"/);
    assert.match(logs, /composition end: id=3 text="你".*value="你b"/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

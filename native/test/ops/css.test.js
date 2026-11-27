'use strict';

// Appservice op replay: Selectors, cascade and component style scopes.
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

checkOpsBuild('appservice ops (css)');

test('appservice ops: component stylesheets do not leak across style scopes', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // glass-easel compiles each component's WXSS into its own style scope and
  // tags classes with it (`addClass(prefix, scope)`, `setStyleScope`).  The
  // app-bar example's `.title{flex:1}` used to leak onto the page's
  // `.title`, collapsing the section headings to zero height so the cards
  // overlapped them.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-style-scope.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // Page-scope `.title`: margin-top 16, text height 24, no app-bar flex rule.
  assert.match(logs, /rect #3 left=0 top=16 w=320 h=24/);
  // The following red box starts after the heading instead of overlapping it.
  assert.match(logs, /rect #5 left=0 top=40 w=320 h=50/);
  // The component-scope `.title{flex:1}` still applies inside its own scope:
  // the flex row's side box is pushed to x=270.
  assert.match(logs, /rect #9 left=270 top=90 w=50 h=40/);
  assert.deepEqual(pixelAt(pixels, 200, 30), [255, 255, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 200, 60), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 300, 100), [0, 170, 0, 255]);
});

test('appservice ops: descendant selectors respect their ancestor compound', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-descendant-'));
  const ops = path.join(dir, 'descendant.ops.json');
  // Two identical rows, only the first one carries `.selected`.  The rule
  // `.item.selected .inner` must only style the first row's child; matching
  // just the last compound turns both children green.
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '.inner { background: #FF0000; } ' +
          '.item.selected .inner { background: #00FF00; }' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3, value: 'width:320px;height:100px;' },
      { op: 'addClass', id: 3, prefix: 'item', name: 1 },
      { op: 'addClass', id: 3, prefix: 'selected', name: 1 },
      { op: 'createElement', id: 4, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 4, value: 'width:320px;height:100px;' },
      { op: 'addClass', id: 4, prefix: 'inner', name: 1 },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 5, value: 'width:320px;height:100px;' },
      { op: 'addClass', id: 5, prefix: 'item', name: 1 },
      { op: 'createElement', id: 6, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 6, value: 'width:320px;height:100px;' },
      { op: 'addClass', id: 6, prefix: 'inner', name: 1 },
      { op: 'appendChild', id: 5, child: 6 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops);
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 160, 50), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 160, 150), [255, 0, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: percent transforms, border triangles and text whitespace', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-popup-'));
  const ops = path.join(dir, 'popup.ops.json');
  // Mirrors the address-book long-press bubble: a circle centred on its row
  // via translateY(-50%), a CSS border triangle whose left side is coloured,
  // and a text node surrounded by template indentation.
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '.bubble{position:absolute;left:20px;top:100px;width:55px;' +
          'height:55px;border-radius:50%;background:#C9C9C9;color:#FFFFFF;' +
          'font-size:50px;line-height:45px;display:flex;align-items:center;' +
          'justify-content:center;transform:translateY(-50%);}' +
          '.arrow{position:absolute;left:110px;top:100px;width:0;height:0;' +
          'border:20px solid;border-color:transparent transparent transparent ' +
          '#FF0000;transform:translateY(-50%);}' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'addClass', id: 3, prefix: 'bubble', name: 1 },
      { op: 'createElement', id: 4, tag: 'text', name: '' },
      { op: 'setText', id: 4, text: '\n                L\n                ' },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
      { op: 'addClass', id: 5, prefix: 'arrow', name: 1 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(ops);
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');

    // translateY(-50%) lifts the 55px circle so its centre lands on y=100.
    assert.deepEqual(pixelAt(pixels, 47, 80), [201, 201, 201, 255]);
    assert.deepEqual(pixelAt(pixels, 30, 115), [201, 201, 201, 255]);
    assert.deepEqual(pixelAt(pixels, 30, 135), [255, 255, 255, 255]);

    // The indented text collapses to "L" and is centred inside the circle.
    let glyph = 0;
    for (let y = 85; y < 115; y++) {
      for (let x = 35; x < 60; x++) {
        const [r, g, b] = pixelAt(pixels, x, y);
        if (r > 230 && g > 230 && b > 230) glyph++;
      }
    }
    assert.ok(glyph > 30, `expected the popup letter, found ${glyph} white pixels`);

    // `width:0;height:0;border:20px` is a 40x40 box whose red left border is
    // a triangle pointing right (x 110..130, y 80..120, apex at 130,100).
    let red = 0;
    let minX = width;
    let maxX = 0;
    let minY = height;
    let maxY = 0;
    for (let y = 60; y < 140; y++) {
      for (let x = 90; x < 160; x++) {
        const [r, g, b] = pixelAt(pixels, x, y);
        if (r > 200 && g < 80 && b < 80) {
          red++;
          minX = Math.min(minX, x);
          maxX = Math.max(maxX, x);
          minY = Math.min(minY, y);
          maxY = Math.max(maxY, y);
        }
      }
    }
    assert.ok(red > 300, `expected the border triangle, found ${red} red pixels`);
    assert.ok(minX >= 108 && minX <= 112, `triangle left edge ${minX}`);
    assert.ok(maxX >= 128 && maxX <= 132, `triangle apex ${maxX}`);
    assert.ok(minY >= 78 && minY <= 82, `triangle top ${minY}`);
    assert.ok(maxY >= 118 && maxY <= 122, `triangle bottom ${maxY}`);
    // The transparent right border stays empty (the triangle points right).
    assert.deepEqual(pixelAt(pixels, 140, 100), [255, 255, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

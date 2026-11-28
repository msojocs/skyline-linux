'use strict';

// Appservice op replay: Text, fonts and inline flow.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const {
  checkOpsBuild,
  countColor,
  height,
  opsSkip,
  pixelAt,
  repoRoot,
  runOps,
  systemFontPath,
  width,
} = require('./helpers');

checkOpsBuild('appservice ops (text)');

test('appservice ops: text padding insets the glyph run inside the content box', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors expanded-scroll-view's tabs: `.tabs-list{display:flex}` with
  // `text{padding:10px 15px}` children.  The paragraph must start at the
  // content-box origin (padding-left 15), not the border-box origin; the
  // element's CSS background must cover the whole border box.  Expected values
  // were cross-checked against the official Windows engine
  // (`appservice-text-padding`, ink x=32..91/156..215 device).
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-text-padding.ops.json');
  const { result, logs, pixels } = runOps(fixture, { dpr: 2 });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  const frameWidth = width * 2;
  const bbox = (xFrom, xTo, predicate) => {
    let x0 = Infinity; let y0 = Infinity; let x1 = -1; let y1 = -1;
    for (let y = 0; y < height * 2; y++) {
      for (let x = xFrom; x < xTo; x++) {
        const offset = (y * frameWidth + x) * 4;
        if (predicate(pixels[offset], pixels[offset + 1], pixels[offset + 2])) {
          if (x < x0) x0 = x;
          if (y < y0) y0 = y;
          if (x > x1) x1 = x;
          if (y > y1) y1 = y;
        }
      }
    }
    return [x0, y0, x1, y1];
  };
  const isInk = (r, g, b) => r < 60 && g < 60 && b < 60;
  const isBackground = (r, g, b) => r === 238 && g === 238 && b === 238;

  // Both 62px (`padding 15+15` + two 16px CJK glyphs) text boxes paint their
  // #EEEEEE background: 0..247 device = 0..123.5 CSS wide, 40px tall.
  assert.deepEqual(bbox(0, frameWidth, isBackground), [0, 0, 247, 79]);

  // Glyph ink starts at 15px padding + ~1px side bearing, i.e. 32 device
  // pixels into each tab.  Vertical ink depends on the installed CJK font's
  // ascent, so only its range is asserted (Windows: y 32..60).
  const first = bbox(0, 124, isInk);
  assert.equal(first[0], 32, `first tab ink starts at ${first[0]}, not at the 15px padding`);
  assert.equal(first[2], 91);
  assert.ok(first[1] >= 24 && first[1] <= 36, `first tab ink top ${first[1]}`);
  assert.ok(first[3] >= 52 && first[3] <= 64, `first tab ink bottom ${first[3]}`);

  // The second flex item starts at x=62 CSS (124 device); its glyphs must be
  // inset by the same padding instead of sitting on its border-box origin.
  const second = bbox(124, 248, isInk);
  assert.deepEqual([second[0], second[2]], [156, 215],
    `second tab ink x=${second[0]}..${second[2]}`);
});

test('appservice ops: @font-face drives the resource-loading channel', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-font.ops.json');
  const resourceRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-res-'));
  const fontBytes = Buffer.from('not-a-real-font');
  fs.writeFileSync(path.join(resourceRoot, 'fake-font.bin'), fontBytes);

  try {
    const { result, logs } = runOps(fixture, {
      debugLogs: true,
      timeoutMs: 2000,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: resourceRoot },
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /appservice font face: TestFont -> fake-font\.bin/);
    assert.match(
      logs,
      /LoadResourceAsync: fake-font\.bin -> .*fake-font\.bin \(ok, 15 bytes, id=\d+, notified=yes\)/,
    );
    // In-memory font state: the family is pending while FontLoader runs, then
    // removed; because the text references it, the runtime schedules a
    // targeted text relayout instead of rebuilding the page.
    assert.match(logs, /AddPendingFont: TestFont \(pending=1\)/);
    assert.match(
      logs,
      /RemovePendingFont: TestFont \(pending=0, used=1, status=-1\)/,
    );
    // The fake bytes are not a valid font, so Dart reports FontFaceLoadStatus
    // error (2) back through NotifyFontFaceLoad after the resource channel
    // completed.
    assert.match(
      logs,
      /NotifyFontFaceLoad: id=\d+ status=2 family=TestFont \(pending=0\)/,
    );
    assert.match(logs, /font relayout: TestFont \(1 text node\(s\)\)/);
  } finally {
    try {
      fs.rmSync(resourceRoot, { recursive: true, force: true });
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: loaded fonts refresh pending text in place', {
  skip: opsSkip || systemFontPath() === null
    ? 'no system TrueType font available' : false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-real-font-'));
  fs.copyFileSync(systemFontPath(), path.join(dir, 'real.ttf'));
  const ops = path.join(dir, 'real-font.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '@font-face{font-family:RealFont;src:url(real.ttf);}' +
          '.p{width:320px;height:240px;background:#FFFFFF;' +
          'font-family:"RealFont", sans-serif;}' +
          '.t{font-size:20px;color:#000000;}' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'addClass', id: 2, prefix: 'p', name: 1 },
      { op: 'createElement', id: 3, tag: 'text', name: '' },
      { op: 'addClass', id: 3, prefix: 't', name: 1 },
      { op: 'setText', id: 3, text: 'Hello RealFont' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      debugLogs: true,
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /appservice font face: RealFont -> real\.ttf/);
    assert.match(logs,
      /LoadResourceAsync: real\.ttf -> .*\(ok, \d+ bytes, id=\d+, notified=yes\)/);
    assert.match(logs, /AddPendingFont: RealFont \(pending=1\)/);
    assert.match(
      logs,
      /RemovePendingFont: RealFont \(pending=0, used=1, status=-1\)/,
    );
    assert.match(
      logs,
      /NotifyFontFaceLoad: id=\d+ status=\d+ family=RealFont \(pending=0\)/,
    );
    // The inherited font-family list reaches the text node, so the completed
    // load re-applies exactly that text widget.
    assert.match(logs, /font relayout: RealFont \(1 text node\(s\)\)/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: repeated font resources hit the native cache', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-rescache-'));
  fs.writeFileSync(path.join(dir, 'shared.bin'), Buffer.from('fake-font-bytes'));
  const ops = path.join(dir, 'cache.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1,
        css: '@font-face{font-family:CachedOne;src:url(shared.bin);}' +
          '@font-face{font-family:CachedTwo;src:url(shared.bin);}' +
          '.page{width:320px;height:240px;background:#FFFFFF;}' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'addClass', id: 2, prefix: 'page', name: 1 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs } = runOps(ops, {
      debugLogs: true,
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const loads = logs.match(/LoadResourceAsync: shared\.bin/g) || [];
    assert.ok(loads.length >= 2,
      `expected both font faces to load, got ${loads.length}`);
    assert.match(logs,
      /LoadResourceAsync: shared\.bin -> .*\(ok, 15 bytes, id=\d+, notified=yes, cache\)/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: bold inline price keeps "0.01 起" on one line', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors `<span class="product-price">¥<text style="font-size:16px;
  // font-weight:bold">0.01</text> 起</span>`.  The glyph approximation must
  // account for the bold face's wider advances; otherwise the 16px run wraps
  // inside its own intrinsic-width box.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-inline-bold-price.ops.json');
  const { result, logs, pixels } = runOps(fixture, { dpr: 2 });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // Bounding box of the red price ink.  A wrapped "0.01" doubles the ink
  // height (the "1" drops to a second line); one line is ~16px tall at dpr=1.
  let minX = Infinity;
  let maxX = -1;
  let minY = Infinity;
  let maxY = -1;
  for (let y = 0; y < height * 2; y++) {
    for (let x = 0; x < width * 2; x++) {
      const [r, g, b] = pixelAt(pixels, x, y);
      if (r > 180 && g < 90 && b < 90) {
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
      }
    }
  }
  assert.ok(maxX > 0, 'no red price ink was painted');
  const inkHeight = maxY - minY + 1;
  assert.ok(inkHeight < 28 * 2,
    `price wrapped onto two lines (ink height ${inkHeight}px): ` +
    `x=${minX}..${maxX} y=${minY}..${maxY}`);
  // The "1" must stay on the same visual line as "0.0".
  assert.ok(maxX - minX > 40,
    `price ink is too narrow; the last digit may be missing: ` +
    `x=${minX}..${maxX}`);
});

test('appservice ops: span/text inline flow keeps the footer row on one line', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // Mirrors the card_transition detail page footer:
  //   <view class="footer"><view class="footer-content">
  //     <input class="footer-input" placeholder="hello"/>
  //     <span><text class="footer-icon">❤️</text> 317</span>
  //     <span><text class="footer-icon">⭐️</text> 723</span>
  //   </view></view>
  // Skyline's `span`/`<text>` wrappers are inline-level, so the two spans keep
  // their intrinsic width and `flex:1` leaves the input most of the row.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-footer-'));
  const fixture = path.join(dir, 'footer.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1, css:
        'page, view, span { box-sizing: border-box; } ' +
        '.footer { font-size: 14px; } ' +
        '.footer-content { display: flex; flex-direction: row; ' +
          'align-items: center; height: 50px; } ' +
        '.footer .footer-input { flex: 1; background-color: #EEEEEE; ' +
          'margin-left: 15px; margin-right: 15px; padding-left: 15px; ' +
          'height: 40px; border-radius: 20px; } ' +
        '.footer span { padding-right: 15px; } ' +
        '.footer-icon { font-size: 26px; }' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'view', name: 'view' },
      { op: 'addClass', id: 3, prefix: 'footer', name: 1 },
      { op: 'createElement', id: 4, tag: 'view', name: 'view' },
      { op: 'addClass', id: 4, prefix: 'footer-content', name: 1 },
      { op: 'createElement', id: 5, tag: 'wx-x', name: 'input' },
      { op: 'addClass', id: 5, prefix: '__wx_display_block__', name: 1 },
      { op: 'addClass', id: 5, prefix: 'footer-input', name: 1 },
      { op: 'createElement', id: 6, tag: 'input', name: 'native-input' },
      { op: 'setStyle', id: 6, value: 'width:100%;height:100%;' },
      { op: 'setAttributes', id: 6,
        attrs: { placeholder: 'hello', cursorColor: '#07C160' } },
      { op: 'appendChild', id: 5, child: 6 },
      { op: 'createElement', id: 7, tag: 'span', name: 'span' },
      { op: 'createElement', id: 8, tag: 'text', name: 'text' },
      { op: 'addClass', id: 8, prefix: 'footer-icon', name: 1 },
      { op: 'createElement', id: 9, tag: 'text', name: '' },
      { op: 'setText', id: 9, text: '❤️' },
      { op: 'appendChild', id: 8, child: 9 },
      { op: 'createElement', id: 10, tag: 'text', name: '' },
      { op: 'setText', id: 10, text: '\n\t\t\t\t\t\t317\n\t\t\t\t\t' },
      { op: 'appendChild', id: 7, child: 8 },
      { op: 'appendChild', id: 7, child: 10 },
      { op: 'createElement', id: 11, tag: 'span', name: 'span' },
      { op: 'createElement', id: 12, tag: 'text', name: 'text' },
      { op: 'addClass', id: 12, prefix: 'footer-icon', name: 1 },
      { op: 'createElement', id: 13, tag: 'text', name: '' },
      { op: 'setText', id: 13, text: '⭐️' },
      { op: 'appendChild', id: 12, child: 13 },
      { op: 'createElement', id: 14, tag: 'text', name: '' },
      { op: 'setText', id: 14, text: '\n\t\t\t\t\t\t723\n\t\t\t\t\t' },
      { op: 'appendChild', id: 11, child: 12 },
      { op: 'appendChild', id: 11, child: 14 },
      { op: 'appendChild', id: 4, child: 5 },
      { op: 'appendChild', id: 4, child: 7 },
      { op: 'appendChild', id: 4, child: 11 },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      timeoutMs: 8000,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const rectOf = (id) => {
      const match = new RegExp(
        `\\[ops-debug\\] rect #${id} left=([-\\d.]+) top=([-\\d.]+) ` +
        'w=([-\\d.]+) h=([-\\d.]+)').exec(logs);
      assert.ok(match, `no rect published for node #${id}:\n${logs}`);
      return {
        left: Number(match[1]), top: Number(match[2]),
        w: Number(match[3]), h: Number(match[4]),
      };
    };

    const input = rectOf(5);
    const span1 = rectOf(7);
    const span2 = rectOf(11);
    const heart = rectOf(9);
    const likes = rectOf(10);
    // `flex:1` gets the remaining row; before the inline defaults the spans
    // measured a full 320px each and the input collapsed to width 0.
    assert.equal(input.left, 15);
    assert.equal(input.h, 40);
    assert.ok(input.w > 120, `input did not grow: ${JSON.stringify(input)}`);
    assert.ok(span1.w < 100, `span kept intrinsic width: ${JSON.stringify(span1)}`);
    assert.ok(span2.w < 100, `span kept intrinsic width: ${JSON.stringify(span2)}`);
    assert.ok(Math.abs(span1.left - (input.left + input.w + 15)) < 0.5,
      `span must follow the input: input=${JSON.stringify(input)} ` +
      `span=${JSON.stringify(span1)}`);
    assert.ok(span2.left + span2.w <= 320.5,
      `second span overflowed: ${JSON.stringify(span2)}`);
    // Heart and `317` share the line box instead of stacking in a column.
    assert.ok(likes.left > heart.left,
      `317 must follow the heart inline: heart=${JSON.stringify(heart)} ` +
      `likes=${JSON.stringify(likes)}`);
    // The U+FE0F variation selector must not add a second 26px advance.
    assert.ok(heart.w < 40,
      `emoji advance includes the variation selector: ${JSON.stringify(heart)}`);
    assert.ok(Math.abs((heart.top + heart.h) - (likes.top + likes.h)) < 1.5,
      `317 must share the line bottom: heart=${JSON.stringify(heart)} ` +
      `likes=${JSON.stringify(likes)}`);
    // The template indentation before the counters collapses to one space
    // between the inline boxes (`<text>⭐️</text>\n 723` -> "⭐️ 723").
    assert.match(logs, /#10 .* text=" 317"/);
    assert.match(logs, /#14 .* text=" 723"/);
    assert.ok(pixels, 'no frame was captured');
    // The native input widget renders the placeholder inside the grey pill.
    const placeholderPixels = countColor(pixels, (r, g, b) =>
      r < 160 && g < 160 && b < 160);
    assert.ok(placeholderPixels > 20,
      `expected placeholder glyphs to paint (dark pixels=${placeholderPixels})`);
    // Emoji-only runs use a color emoji primary family (the Skia
    // FontCollection/SkFontMgr patches).  U+2764 is present in DejaVu as a
    // black glyph, so this asserts the primary family actually reached the
    // color font rather than the default text font.
    const colorEmojiFont = [
      '/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf',
      '/usr/share/fonts/noto/NotoColorEmoji.ttf',
    ].some((candidate) => fs.existsSync(candidate));
    if (colorEmojiFont) {
      const red = countColor(pixels, (r, g, b) =>
        r > 150 && g < 110 && b < 110);
      assert.ok(red > 50,
        `expected a color emoji heart (red pixels=${red})`);
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: text ink is not clipped at the measured line box', {
  skip: opsSkip,
  timeout: 90000,
}, () => {
  // card_transition's detail paragraphs start with a color emoji.  The emoji
  // font metrics make that line taller than the replay's 1.2em estimate, so a
  // two-line paragraph's last line paints below the text node's box.  Skyline
  // sizes the box and lets ink overflow it (an explicit `height` does not clip
  // the glyphs either), so the default `text-overflow` must stay `visible`;
  // Flutter's `clip` shaved the bottom of every paragraph flat.
  const colorEmojiFont = [
    '/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf',
    '/usr/share/fonts/noto/NotoColorEmoji.ttf',
  ].some((candidate) => fs.existsSync(candidate));
  if (!colorEmojiFont) {
    // Without the emoji font the line metrics match the estimate and there is
    // no ink to assert.
    return;
  }
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-text-ink-'));
  const writeOps = (name, textOverflow) => {
    const fixture = path.join(dir, `${name}.ops.json`);
    fs.writeFileSync(fixture, JSON.stringify({
      ops: [
        { op: 'appendStyleSheet', index: 1, css:
          'page { background: #FFFFFF; } ' +
          '.box { padding: 10px; } ' +
          '.p { width: 300px; font-size: 16px; color: #000000; ' +
            (textOverflow ? `text-overflow: ${textOverflow}; ` : '') + '}' },
        { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
        { op: 'createElement', id: 3, tag: 'view', name: 'view' },
        { op: 'addClass', id: 3, prefix: 'box', name: 1 },
        { op: 'createElement', id: 4, tag: 'text', name: '' },
        { op: 'addClass', id: 4, prefix: 'p', name: 1 },
        { op: 'setText', id: 4,
          text: '🌟 无需为每个页面新建一个 JS 引擎实例（WebView），' +
            '减少了内存、时间开销' },
        { op: 'appendChild', id: 3, child: 4 },
        { op: 'appendChild', id: 2, child: 3 },
        { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
        { op: 'appendChild', id: 1, child: 2 },
        { op: 'attach', root: 1 },
        { op: 'startRender' },
      ],
    }));
    return fixture;
  };

  const dpr = 2;
  const inkBelowBox = (run) => {
    const { logs, pixels } = run;
    const match = /\[ops-debug\] rect #4 left=([-\d.]+) top=([-\d.]+) w=([-\d.]+) h=([-\d.]+)/
      .exec(logs);
    assert.ok(match, `no rect published for the text node:\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    const top = Number(match[2]);
    const boxHeight = Number(match[4]);
    const firstRow = Math.ceil((top + boxHeight) * dpr);
    let dark = 0;
    for (let y = firstRow; y < firstRow + 6; y++) {
      for (let x = 0; x < width * dpr; x++) {
        const offset = (y * width * dpr + x) * 4;
        if (pixels[offset] < 128 && pixels[offset + 1] < 128 &&
            pixels[offset + 2] < 128) {
          dark++;
        }
      }
    }
    return { dark, firstRow };
  };

  try {
    const visibleRun = runOps(writeOps('visible', ''), {
      dpr, timeoutMs: 15000, env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(visibleRun.result.status, 0,
      `skyline_host failed (status ${visibleRun.result.status}):\n` +
      visibleRun.logs);
    const visible = inkBelowBox(visibleRun);
    assert.ok(visible.dark > 20,
      `default text-overflow clipped the last line's ink ` +
      `(dark rows=${visible.dark}, firstRow=${visible.firstRow})`);

    // Control: an explicit `text-overflow: clip` still clips at the box, so
    // the assertion above really observes the default (visible) behaviour.
    const clipRun = runOps(writeOps('clip', 'clip'), {
      dpr, timeoutMs: 15000, env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(clipRun.result.status, 0,
      `skyline_host failed (status ${clipRun.result.status}):\n` +
      clipRun.logs);
    const clip = inkBelowBox(clipRun);
    assert.equal(clip.dark, 0,
      `explicit text-overflow:clip must clip at the box ` +
      `(dark rows=${clip.dark}, firstRow=${clip.firstRow})`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: container text-overflow reaches the raw text child', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // WXSS declares the ellipsis on the element that owns the text:
  //   <view class="box">AAAA BBBB CCCC DDDD</view>
  // The replay materialises the raw text as a child text node, so the style
  // must be carried down; otherwise the line is cut mid-glyph with no "…"
  // (Flutter's paragraph only sees the child's own style).
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-ellipsis-'));
  const fixture = path.join(dir, 'ellipsis.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1, css:
        'page { background: #FFFFFF; } ' +
        '.box { width: 120px; height: 24px; font-size: 16px; ' +
          'color: #000000; overflow: hidden; white-space: nowrap; ' +
          'text-overflow: ellipsis; margin-bottom: 6px; } ' +
        '.clip { text-overflow: clip; }' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'createElement', id: 3, tag: 'view', name: 'view' },
      { op: 'addClass', id: 3, prefix: 'box', name: 1 },
      { op: 'createElement', id: 4, tag: 'text', name: '' },
      { op: 'setText', id: 4, text: 'AAAA BBBB CCCC DDDD' },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'createElement', id: 5, tag: 'view', name: 'view' },
      { op: 'addClass', id: 5, prefix: 'box', name: 1 },
      { op: 'addClass', id: 5, prefix: 'clip', name: 1 },
      { op: 'createElement', id: 6, tag: 'text', name: '' },
      { op: 'setText', id: 6, text: 'AAAA BBBB CCCC DDDD' },
      { op: 'appendChild', id: 5, child: 6 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));

  const dpr = 2;
  const { result, logs, pixels } = runOps(fixture, {
    dpr, timeoutMs: 15000, env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });
  try {
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    const rectOf = (id) => {
      const match = new RegExp(
        `\\[ops-debug\\] rect #${id} left=([-\\d.]+) top=([-\\d.]+) ` +
        'w=([-\\d.]+) h=([-\\d.]+)').exec(logs);
      assert.ok(match, `no rect published for node #${id}:\n${logs}`);
      return {
        left: Number(match[1]), top: Number(match[2]),
        w: Number(match[3]), h: Number(match[4]),
      };
    };
    // Rightmost inked column inside the text box's own band.
    const inkRight = (text) => {
      const x0 = Math.round(text.left * dpr);
      const x1 = Math.round((text.left + text.w) * dpr);
      const y0 = Math.round(text.top * dpr);
      const y1 = Math.round((text.top + text.h) * dpr);
      let right = null;
      for (let x = x0; x < x1; x++) {
        for (let y = y0; y < y1; y++) {
          const offset = (y * width * dpr + x) * 4;
          if (pixels[offset] < 128 && pixels[offset + 1] < 128 &&
              pixels[offset + 2] < 128) {
            right = x;
            break;
          }
        }
      }
      return { right, edge: x1 };
    };

    const ellipsisInk = inkRight(rectOf(4));
    const clipInk = inkRight(rectOf(6));
    assert.ok(ellipsisInk.right !== null,
      'ellipsis text painted nothing');
    // Flutter's ellipsis reserves room before the clip edge; a plain clip
    // keeps drawing right up to (and into) the edge.
    assert.ok(ellipsisInk.edge - ellipsisInk.right >= 8,
      `inherited text-overflow:ellipsis did not add an ellipsis ` +
      `(rightmost ink=${ellipsisInk.right}, edge=${ellipsisInk.edge})`);
    assert.ok(clipInk.edge - clipInk.right <= 3,
      `text-overflow:clip should reach the edge ` +
      `(rightmost ink=${clipInk.right}, edge=${clipInk.edge})`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: auto-width flex items use their text max-content width', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  // card_transition's detail navigation bar: the `flex:1` title must claim the
  // remaining row, and the "follow" pill (auto width + margin-right) must keep
  // its text width instead of stretching to the whole row.  Before the fix the
  // pill measured its block text child at the full available width, so its
  // `flex-basis:auto` became the row and the title collapsed to 0.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-navbar-'));
  const fixture = path.join(dir, 'navbar.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1, css:
        'view { box-sizing: border-box; } ' +
        '.row { display: flex; flex-direction: row; align-items: center; ' +
          'padding: 0 20px; height: 44px; font-size: 14px; } ' +
        '.avatar { width: 24px; height: 24px; background: #3366FF; } ' +
        '.title { flex: 1; padding-left: 8px; } ' +
        '.follow { border: 1px solid #FF0000; border-radius: 15px; ' +
          'padding: 3px 15px; margin-right: 80px; }' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'view', name: 'view' },
      { op: 'addClass', id: 3, prefix: 'row', name: 1 },
      { op: 'createElement', id: 4, tag: 'view', name: 'view' },
      { op: 'addClass', id: 4, prefix: 'avatar', name: 1 },
      { op: 'createElement', id: 5, tag: 'view', name: 'view' },
      { op: 'addClass', id: 5, prefix: 'title', name: 1 },
      { op: 'createElement', id: 6, tag: 'text', name: '' },
      { op: 'setText', id: 6, text: 'BINNIE' },
      { op: 'appendChild', id: 5, child: 6 },
      { op: 'createElement', id: 7, tag: 'view', name: 'view' },
      { op: 'addClass', id: 7, prefix: 'follow', name: 1 },
      { op: 'createElement', id: 8, tag: 'text', name: '' },
      { op: 'setText', id: 8, text: 'Follow' },
      { op: 'appendChild', id: 7, child: 8 },
      { op: 'appendChild', id: 3, child: 4 },
      { op: 'appendChild', id: 3, child: 5 },
      { op: 'appendChild', id: 3, child: 7 },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs } = runOps(fixture, {
      timeoutMs: 3000,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const rectOf = (id) => {
      const match = new RegExp(
        `\\[ops-debug\\] rect #${id} left=([-\\d.]+) top=([-\\d.]+) ` +
        'w=([-\\d.]+) h=([-\\d.]+)').exec(logs);
      assert.ok(match, `no rect published for node #${id}:\n${logs}`);
      return {
        left: Number(match[1]), top: Number(match[2]),
        w: Number(match[3]), h: Number(match[4]),
      };
    };
    const avatar = rectOf(4);
    const title = rectOf(5);
    const follow = rectOf(7);
    assert.equal(avatar.left, 20);
    assert.ok(title.w > 50,
      `flex:1 title collapsed: ${JSON.stringify(title)}`);
    assert.ok(follow.w < 100,
      `auto-width pill stretched: ${JSON.stringify(follow)}`);
    assert.ok(follow.left >= title.left + title.w - 0.5,
      `pill must follow the title: title=${JSON.stringify(title)} ` +
      `follow=${JSON.stringify(follow)}`);
    // padding 20 + 80px margin keeps the pill's right edge at 320-100.
    assert.ok(follow.left + follow.w <= 220.5,
      `pill ignores margin-right: ${JSON.stringify(follow)}`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: CJK text resolves through the font fallback list', {
  skip: opsSkip,
  timeout: 60000,
}, (t) => {
  const fonts = spawnSync('fc-list', [':lang=zh'], { encoding: 'utf8' });
  if (fonts.status !== 0 || !String(fonts.stdout || '').trim()) {
    t.skip('no CJK fonts installed');
    return;
  }
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-cjk-'));
  const ops = path.join(dir, 'cjk.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'text', name: 'text' },
      { op: 'setText', id: 3, text: '你好' },
      { op: 'setStyle', id: 3, value: 'font-size:48px;color:#000000;' },
      { op: 'appendChild', id: 2, child: 3 },
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
    const dark = countColor(pixels,
      (r, g, b) => r < 100 && g < 100 && b < 100);
    assert.ok(dark > 300, `expected CJK glyph pixels, found ${dark}`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: isolated inline text wraps at the container width', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-inline-text-wrap.ops.json');
  const { result, logs, pixels } = runOps(fixture, {
    timeoutMs: 1800,
    frames: 0,
  });
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // The long card's title is a single `<text>` element inside a 100%-wide
  // block.  It must wrap at the card width (3 lines) instead of shrink-
  // wrapping to its intrinsic width and being clipped.
  assert.deepEqual(pixelAt(pixels, 12, 45), [255, 255, 255, 255],
    'long title card grows past the first line');
  assert.deepEqual(pixelAt(pixels, 12, 75), [0, 255, 0, 255],
    'long title card ends after the wrapped lines');
  let secondLineInk = 0;
  for (let y = 31; y < 52; y++) {
    for (let x = 10; x < 130; x++) {
      const [r, g, b] = pixelAt(pixels, x, y);
      if (r < 80 && g < 80 && b < 80) secondLineInk++;
    }
  }
  assert.ok(secondLineInk > 20, `no glyphs on the wrapped line (${secondLineInk})`);

  // The short title stays a one-line box, so the same y is page background.
  assert.deepEqual(pixelAt(pixels, 152, 45), [0, 255, 0, 255],
    'short title card keeps one line');
});

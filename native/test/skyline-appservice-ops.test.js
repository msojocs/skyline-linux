'use strict';

// Phase-3 appservice/V8 operation replay.
//
// `tools/record-skyline-ops.js` runs the official Skyline renderer bundle
// (exparser + skyline-js) in Node's V8 and captures the node-operation stream
// that the bundle would send to `skyline.node`'s native bindings.  The embedded
// runtime (native/runtime/appservice_*.cc) replays that stream through
// the real `package:skyline` callbacks, replacing the hand-written widget
// descriptors.
//
// Tests:
//   * a committed operation fixture (no official bundle required) drives three
//     CSS-styled stripes;
//   * the real renderer bundle is recorded on demand (skipped when the DevTools
//     resources are not installed) and its page is rendered.
//
// Skips when the engine/host/kernel are missing.

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync, spawn } = require('node:child_process');
const { checkBuildCurrent, devtoolsEntrypointArgs } = require('../../test/lib/host');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build', 'skyline_host');
const engineDir = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-debug');
const engineLib = path.join(engineDir, 'libflutter_engine.so');
const engineRuntime = path.join(engineDir, 'libskyline.so');
const engineIcu = path.join(engineDir, 'icudtl.dat');
const toolchainIcu = path.join(
  repoRoot, 'native', 'engine', 'toolchain', 'engine-artifacts', 'icudtl.dat',
);
const icu = fs.existsSync(engineIcu) ? engineIcu : toolchainIcu;
const assets = path.join(
  repoRoot, 'native', 'engine', 'work', 'skyline-app', 'build', 'flutter_assets',
);
const kernel = path.join(assets, 'kernel_blob.bin');
const recorder = path.join(repoRoot, 'tools', 'record-skyline-ops.js');

const width = 320;
const height = 240;
const frameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host (npm run native:build)');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so (npm run engine:build)');
  if (!fs.existsSync(engineRuntime)) {
    missing.push('embedded runtime (bash native/engine/patch-skyline-runtime.sh)');
  }
  if (!fs.existsSync(kernel)) missing.push('recompiled kernel (npm run kernel:skyline)');
  if (!fs.existsSync(icu)) missing.push('icudtl.dat');
  return missing.length ? missing.join('; ') : null;
}

function parseResult(stdout) {
  const match = /^SKYLINE_HOST_RESULT (.*)$/m.exec(stdout || '');
  if (!match) return null;
  try {
    return JSON.parse(match[1]);
  } catch {
    return null;
  }
}

function runHost(opsPath, options = {}) {
  const key = `skyline_ops_${process.pid}_${Date.now().toString(36)}_${Math.random()
    .toString(36)
    .slice(2, 8)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  // Frames are rendered at `width x height` CSS pixels; `--dpr` scales the
  // shared-memory frame, which is useful for glyph-level assertions.
  const dpr = options.dpr || 1;
  const expectedFrameSize = width * height * 4 * dpr * dpr;
  const env = {
    ...process.env,
    ...(options.env || {}),
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_APP_SERVICE_OPS: `@${opsPath}`,
  };
  const result = spawnSync(host, [
    '--assets', assets,
    '--icu', icu,
    '--width', String(width),
    '--height', String(height),
    '--dpr', String(dpr),
    '--shm-key', key,
    '--timeout-ms', String(options.timeoutMs || 20000),
    ...(options.frames === undefined ? [] : ['--frames', String(options.frames)]),
    ...(options.args || []),
    '--no-shutdown',
    ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
  ], { encoding: 'utf8', timeout: 45000, env });

  const details = parseResult(result.stdout);
  const diagnostics = [
    result.stdout && result.stdout.trim(),
    result.stderr && result.stderr.trim(),
  ].filter(Boolean).join('\n');

  let pixels = null;
  try {
    if (fs.existsSync(shmPath)) {
      const buffer = fs.readFileSync(shmPath);
      const dataSize = Number(buffer.readBigUInt64LE(0));
      assert.equal(buffer.length, 16 + dataSize);
      const data = buffer.subarray(16);
      assert.equal(data.readUInt32LE(4), expectedFrameSize);
      pixels = data.subarray(8, 8 + expectedFrameSize);
    }
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
  return { result, logs: diagnostics, details, pixels };
}

function pixelAt(pixels, x, y) {
  const offset = (y * width + x) * 4;
  return [...pixels.subarray(offset, offset + 4)];
}

function countColor(pixels, predicate) {
  let count = 0;
  for (let i = 0; i < pixels.length; i += 4) {
    if (predicate(pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3])) count++;
  }
  return count;
}

// Writes an 8x8 uncompressed 24-bit BMP with four colored quadrants:
// red / green on top, blue / yellow at the bottom.
function writeQuadBmp(file) {
  const width = 8;
  const height = 8;
  const rowBytes = width * 3;
  const pixels = Buffer.alloc(rowBytes * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const top = y < height / 2;
      const left = x < width / 2;
      let r = 0;
      let g = 0;
      let b = 0;
      if (top && left) {
        r = 255;
      } else if (top) {
        g = 255;
      } else if (left) {
        b = 255;
      } else {
        r = 255;
        g = 255;
      }
      const offset = (height - 1 - y) * rowBytes + x * 3;
      pixels[offset] = b;
      pixels[offset + 1] = g;
      pixels[offset + 2] = r;
    }
  }
  const header = Buffer.alloc(54);
  header.write('BM', 0);
  header.writeUInt32LE(54 + pixels.length, 2);
  header.writeUInt32LE(54, 10);
  header.writeUInt32LE(40, 14);
  header.writeInt32LE(width, 18);
  header.writeInt32LE(height, 22);
  header.writeUInt16LE(1, 26);
  header.writeUInt16LE(24, 28);
  header.writeUInt32LE(0, 30);
  header.writeUInt32LE(pixels.length, 34);
  fs.writeFileSync(file, Buffer.concat([header, pixels]));
}

// First usable TrueType font for the in-memory font tests.
function systemFontPath() {
  const candidates = [
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
    '/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
  ];
  return candidates.find((candidate) => fs.existsSync(candidate)) || null;
}

function recordRealOps(options = {}) {
  if (!fs.existsSync(recorder)) return null;
  const suffix = options.tapOption === undefined ? '' : `-tap-${options.tapOption}`;
  const textSuffix = options.tapText === undefined ? '' : '-tap-text';
  const out = path.join(
    os.tmpdir(), `skyline-ops-${process.pid}${suffix}${textSuffix}.json`);
  const args = [recorder, '--out', out, '--quiet'];
  if (options.tapOption !== undefined) {
    args.push('--tap-option', String(options.tapOption));
  }
  if (options.tapText !== undefined) {
    args.push('--tap-text', String(options.tapText));
  }
  if (options.bundle !== undefined) {
    args.push('--bundle', options.bundle);
  }
  if (options.animateMs !== undefined) {
    args.push('--animate-ms', String(options.animateMs));
  }
  const result = spawnSync(process.execPath, args, {
    encoding: 'utf8',
    timeout: 30000,
  });
  if (result.status !== 0 || !fs.existsSync(out)) return null;
  try {
    return { path: out, payload: JSON.parse(fs.readFileSync(out, 'utf8')) };
  } catch {
    return null;
  }
}

function legacyBundle() {
  const resources = [
    process.env.SKYLINE_WIN_RESOURCES,
    path.resolve(repoRoot, '..', 'wechat-web-devtools-linux', 'cache',
      'devtools', 'resources', 'app.asar.unpacked'),
    path.resolve(repoRoot, '..', 'wechat-web-devtools-linux', 'tmp',
      'official-win-resources', 'resources', 'app.asar.unpacked'),
  ].filter(Boolean);
  for (const resource of resources) {
    const candidate = path.join(resource, 'node_modules', 'skyline-addon',
      'bundle', 'flutter_assets', 'assets', 'app_0.js');
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

test('appservice ops: build artifacts match the sources', {
  skip: prerequisiteSkip() || false,
}, t => {
  checkBuildCurrent(t, ['engine-debug', 'kernel']);
});

test('appservice ops: committed stripe fixture replays through the runtime', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-stripes.ops.json');
  const { result, logs, pixels } = runHost(fixture);

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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
  skip: prerequisiteSkip() || false,
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
  const { result, logs, pixels } = runHost(fixture, { dpr: 2 });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: text padding insets the glyph run inside the content box', {
  skip: prerequisiteSkip() || false,
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
  const { result, logs, pixels } = runHost(fixture, { dpr: 2 });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: app-bar context installs an overlay, not a page route', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // Mirrors `app.json#appBar`: the SDK creates a second render context and
  // attaches it through `attachAppBar`.  The runtime must hand the tree to
  // Dart `CreateAppBar` (window overlay) instead of materialising another
  // route, otherwise the app bar paints on top of the page as a full page.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-appbar.ops.json');
  const { result, logs, pixels } = runHost(fixture, { dpr: 2 });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: component stylesheets do not leak across style scopes', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // glass-easel compiles each component's WXSS into its own style scope and
  // tags classes with it (`addClass(prefix, scope)`, `setStyleScope`).  The
  // app-bar example's `.title{flex:1}` used to leak onto the page's
  // `.title`, collapsing the section headings to zero height so the cards
  // overlapped them.
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-style-scope.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: defaultContentBox renderer flag stacks height and padding', {
  skip: prerequisiteSkip() || false,
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
  const { result, logs, pixels } = runHost(fixture, {
    dpr: 2,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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
  skip: prerequisiteSkip() || false,
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
  const { result, logs } = runHost(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  // Content-box height: 42 + 47 padding.
  assert.match(logs, /rect #4 left=0 top=0 w=320 h=89/);
  // Left slot width 80 + padding-left 16.
  assert.match(logs, /rect #5 left=0 top=47 w=96 h=42/);
  // Centre clamped to the line: 320 - 80 padding - 96 left slot = 144.
  assert.match(logs, /rect #6 left=96 top=47 w=144 h=42/);
});

test('appservice ops: calc() and env() resolve in box edges', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // app-bar's `.scroll-area` reserves room for the mini player with
  // `margin-bottom: calc(84px + env(safe-area-inset-bottom))`; dropping the
  // declaration let the list run under the app bar and hid the last cards.
  // Verified against the Windows engine (`appservice-calc-env`, inset 0 here).
  const fixture = path.join(repoRoot, 'test', 'fixtures',
    'appservice-calc-env.ops.json');
  const { result, logs } = runHost(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /rect #3 left=0 top=0 w=320 h=120/);
  // 120 + calc(40px + 0px) env bottom.
  assert.match(logs, /rect #4 left=0 top=160 w=320 h=20/);
});

test('appservice ops: descendant selectors respect their ancestor compound', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops);
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops);
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

test('appservice ops: removing a background repaints without a rebuild', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, { timeoutMs: 2000, frames: 0 });
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-update.ops.json');
  const { result, logs, pixels, details } = runHost(fixture, {
    timeoutMs: 2500,
    frames: 0,
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-text-update.ops.json');
  const { result, logs, pixels, details } = runHost(fixture, {
    timeoutMs: 2000,
    frames: 0,
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: @font-face drives the resource-loading channel', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-font.ops.json');
  const resourceRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-res-'));
  const fontBytes = Buffer.from('not-a-real-font');
  fs.writeFileSync(path.join(resourceRoot, 'fake-font.bin'), fontBytes);

  try {
    const { result, logs } = runHost(fixture, {
      timeoutMs: 2000,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: resourceRoot },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || systemFontPath() === null
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
    const { result, logs } = runHost(ops, {
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
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

test('appservice ops: scrollview renders clipped content', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll-static.ops.json');
  const { result, logs, pixels } = runHost(fixture);

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // Mirrors associated-scroll-view's `<scroll-view scroll-x type="list">`:
  // the renderer creates the inner `scrollview` element without the user's
  // `display:flex;flex-direction:row`, so the runtime derives the flow
  // direction from `scrollX`.  Intrinsic 70x90 items must sit side by side.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-scroll-horizontal.ops.json');
  const { result, logs, pixels } = runHost(fixture);

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: swiper height:auto overrides the builtin 150px base rule', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // Mirrors `.product-list-wrapper { flex: 1; height: auto; background:
  // white }` inside a flex row.  The base stylesheet's `swiper{height:150px}`
  // must lose to the class's `auto`, the swiper must stretch to the row
  // height, and its CSS background must paint.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-swiper-auto-height.ops.json');
  const { result, logs, pixels } = runHost(fixture);

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');

  // The 60px cell paints blue at the top of the stretched swiper; the rest of
  // the swiper box is its red background (not the green parent row).
  assert.deepEqual(pixelAt(pixels, 160, 10), [0, 0, 255, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 100), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 239), [255, 0, 0, 255]);
});

test('appservice ops: scroll-view at its edge yields the drag to the parent swiper', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      timeoutMs: 3000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: bold inline price keeps "0.01 起" on one line', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // Mirrors `<span class="product-price">¥<text style="font-size:16px;
  // font-weight:bold">0.01</text> 起</span>`.  The glyph approximation must
  // account for the bold face's wider advances; otherwise the 16px run wraps
  // inside its own intrinsic-width box.
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-inline-bold-price.ops.json');
  const { result, logs, pixels } = runHost(fixture, { dpr: 2 });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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

test('appservice ops: ScrollViewUpdateScrollTop scrolls the content', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-scroll.ops.json');
  const { result, logs, pixels } = runHost(fixture, { timeoutMs: 2000, frames: 0 });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.match(logs, /appservice setScrollTop: id=3 value=80/);
  assert.ok(pixels, 'no frame was captured');

  // After jumping to 80px, red is 0..19 and green starts at y=20.
  assert.deepEqual(pixelAt(pixels, 160, 10), [255, 0, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 20), [0, 255, 0, 255]);
  assert.deepEqual(pixelAt(pixels, 160, 100), [0, 255, 0, 255]);
});

test('appservice ops: host drag gesture scrolls the scroll view', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: swiper callbacks and programmatic paging', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 3000,
      frames: 0,
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, { timeoutMs: 2500, frames: 0 });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: span/text inline flow keeps the footer row on one line', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 8000,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const visibleRun = runHost(writeOps('visible', ''), {
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
    const clipRun = runHost(writeOps('clip', 'clip'), {
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
  skip: prerequisiteSkip() || false,
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
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      timeoutMs: 3000,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: refresher programmatic pull drives callbacks', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
      timeoutMs: 5000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /refresher trigger: id=3 open=1 twoLevel=0/);
    assert.match(logs, /refresher pulling: id=3 dy=[\d.]+/);
    assert.match(logs, /refresher status: id=3 mode=2 dy=[\d.]+/);
    assert.match(logs, /refresher refresh: id=3 dy=[\d.]+/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: intersection observer, selection range and back interception', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
      timeoutMs: 3000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
      timeoutMs: 6500,
      frames: 0,
      env: {
        SKYLINE_APP_SERVICE_DEBUG: '1',
        SKYLINE_SNAPSHOT_DIR: snapshotDir,
      },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('host pointer script reaches the Skyline hit test path', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: list-view renders a virtualized sliver', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-listview.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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
    const scrolled = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-gridview.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });

  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: animated transforms republish hit-test rects', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      timeoutMs: 1200,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: image nodes paint through NewDirectRenderWidget', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-image.ops.json');
  const resourceRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-img-'));
  writeQuadBmp(path.join(resourceRoot, 'quad.bmp'));
  try {
    const { result, logs, pixels } = runHost(fixture, {
      env: { SKYLINE_RESOURCE_ROOT: resourceRoot, SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /image #3: quad\.bmp -> .*quad\.bmp \(246 bytes, 8x8\)/);
    assert.match(logs, /image paint: .* 8x8 size=320x240 at \(0,0\)/);
    assert.ok(pixels, 'no frame was captured');

    // The 8x8 quadrants are stretched to the 320x240 image node.
    assert.deepEqual(pixelAt(pixels, 80, 60), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 60), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 80, 180), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 180), [255, 255, 0, 255]);
  } finally {
    try {
      fs.rmSync(resourceRoot, { recursive: true, force: true });
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: image border-radius clips the bitmap and keeps the border', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  // Mirrors associated-scroll-view's category avatar:
  //   <image class="avatar" mode="aspectFill" src="..."/>
  //   .avatar { width:50px; height:50px; border-radius:100%;
  //             border:2px solid white; }
  // At dpr=1 the 100x100 box is circular; the bitmap must be clipped and the
  // 2px border must still paint (the clip sits inside the decoration).
  const resourceRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-avatar-'));
  writeQuadBmp(path.join(resourceRoot, 'quad.bmp'));
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-avatar-ops-'));
  const fixture = path.join(dir, 'avatar.ops.json');
  fs.writeFileSync(fixture, JSON.stringify({
    ops: [
      { op: 'appendStyleSheet', index: 1, css:
        '.page { width: 320px; height: 240px; background: #00FF00; } ' +
        '.avatar { position: absolute; left: 20px; top: 20px; ' +
        'width: 100px; height: 100px; border-radius: 50%; ' +
        'border: 2px solid #FFFFFF; }' },
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'addClass', id: 2, prefix: 'page', name: 1 },
      { op: 'createElement', id: 3, tag: 'image', name: 'image' },
      { op: 'addClass', id: 3, prefix: 'avatar', name: 1 },
      { op: 'setAttributes', id: 3,
        attrs: { src: 'quad.bmp', mode: 'aspectFill' } },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(fixture, {
      env: { SKYLINE_RESOURCE_ROOT: resourceRoot },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');

    // Centre of the circle: top-left quadrant of the quad bitmap.
    assert.deepEqual(pixelAt(pixels, 60, 60), [255, 0, 0, 255]);
    // Box corner lies outside the 50px radius: the page shows through.
    assert.deepEqual(pixelAt(pixels, 24, 24), [0, 255, 0, 255]);
    // The 2px white border ring is painted inside the border box.
    assert.deepEqual(pixelAt(pixels, 70, 21), [255, 255, 255, 255]);
    // Just inside the border the clipped bitmap starts.
    assert.deepEqual(pixelAt(pixels, 60, 24), [255, 0, 0, 255]);
  } finally {
    try {
      fs.rmSync(resourceRoot, { recursive: true, force: true });
      fs.rmSync(dir, { recursive: true, force: true });
    } catch {
      // Best effort.
    }
  }
});

test('appservice ops: stretch cross size subtracts cross-axis margins', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      timeoutMs: 8000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: ForwardHttpGet bridges to RequestHttpResource', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-stripes.ops.json');
  // The server must live in a separate process: runHost() blocks this event
  // loop with spawnSync.
  const server = spawn(process.execPath, ['-e', `
    const http = require('http');
    const server = http.createServer((req, res) => {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('skyline-http-ok');
    });
    server.listen(0, '127.0.0.1', () => console.log(server.address().port));
  `], { stdio: ['ignore', 'pipe', 'inherit'] });
  try {
    const port = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('http server did not start')), 5000);
      server.stdout.once('data', (data) => {
        clearTimeout(timer);
        resolve(Number(String(data).trim()));
      });
    });

    const { result, logs } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: {
        SKYLINE_HTTP_TEST_URL: `http://127.0.0.1:${port}/`,
        SKYLINE_APP_SERVICE_DEBUG: '1',
      },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /ForwardHttpGet: id=\d+ url=http:\/\/127\.0\.0\.1:\d+\//);
    assert.match(logs, /SKYLINE_HTTP_RESULT 15 skyline-http-ok/);
  } finally {
    server.kill();
  }
});

test('appservice ops: mouse drag scrolls the scroll view', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: input and route channels receive Dart events', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: real exparser bundle renders through the V8 capture', {
  skip: prerequisiteSkip() || false,
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

    const { result, logs, pixels } = runHost(recorded.path);
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
  skip: prerequisiteSkip() || false,
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

    const { result, logs, pixels, details } = runHost(recorded.path, {
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

test('appservice ops: list-builder mounts items on demand', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-builder.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: route stack pushes and pops pages', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-route.ops.json');
  const { result, logs, pixels, details } = runHost(fixture, {
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

test('appservice ops: image objectFit contains the bitmap', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-fit-'));
  const bmp = path.join(dir, 'quad.bmp');
  const ops = path.join(dir, 'fit.ops.json');
  writeQuadBmp(bmp);
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#808080;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'image' },
      { op: 'setStyle', id: 3, value: 'width:200px;height:100px;' },
      { op: 'setAttribute', id: 3, name: 'src', value: 'quad.bmp' },
      { op: 'setAttribute', id: 3, name: 'mode', value: 'aspectFit' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 1500,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');
    // 8x8 bitmap scaled to 100x100, centered in the 200x100 box.
    assert.deepEqual(pixelAt(pixels, 10, 50), [128, 128, 128, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 10), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 140, 10), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 90), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 140, 90), [255, 255, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: network images load through RequestHttpResource', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-net-'));
  const bmp = path.join(dir, 'quad.bmp');
  writeQuadBmp(bmp);
  const encoded = fs.readFileSync(bmp).toString('base64');
  // The server must live in a separate process: runHost() blocks this event
  // loop with spawnSync.
  const server = spawn(process.execPath, ['-e', `
    const http = require('http');
    const bytes = Buffer.from('${encoded}', 'base64');
    const server = http.createServer((req, res) => {
      res.writeHead(200, {
        'Content-Type': 'image/bmp',
        'Content-Length': String(bytes.length),
      });
      res.end(bytes);
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
    const ops = path.join(dir, 'network.ops.json');
    fs.writeFileSync(ops, JSON.stringify({
      ops: [
        { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
        { op: 'setStyle', id: 2, value: 'width:320px;height:240px;' },
        { op: 'createElement', id: 3, tag: 'empty', name: 'image' },
        { op: 'setStyle', id: 3, value: 'width:320px;height:240px;' },
        { op: 'setAttribute', id: 3, name: 'src',
          value: `http://127.0.0.1:${port}/quad.bmp` },
        { op: 'appendChild', id: 2, child: 3 },
        { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
        { op: 'appendChild', id: 1, child: 2 },
        { op: 'attach', root: 1 },
        { op: 'startRender' },
      ],
    }));
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 3000,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /image #3: fetch http:\/\/127\.0\.0\.1:\d+\/quad\.bmp/);
    assert.match(logs, /network image \(\d+ bytes\)/);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 80, 60), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 60), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 80, 180), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 240, 180), [255, 255, 0, 255]);
  } finally {
    server.kill();
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: host key events drive the keyboard channel', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, details } = runHost(ops, {
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

test('appservice ops: recorded SPA navigation replays a route stack', {
  skip: prerequisiteSkip() || false,
  timeout: 90000,
}, () => {
  const bundle = legacyBundle();
  if (bundle === null) return;  // official renderer bundle not installed
  const recorded = recordRealOps({ bundle, tapText: 'Jump To PageB' });
  if (recorded === null) return;
  try {
    assert.ok((recorded.payload.updates || []).length >= 1,
      'tap should emit a page-B operation batch');
    const { result, logs, pixels } = runHost(recorded.path, {
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

test('appservice ops: absolute nodes use the parent content box', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-absolute.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: CSS transforms paint without affecting flex flow', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-shadow.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: radius and root style updates stay targeted', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
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

test('appservice ops: CSS gradients build and update in place', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
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

test('appservice ops: fixed nodes stay pinned inside a scrollview', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-fixed-scroll.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: transform-origin changes the transform pivot', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform-origin.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: align-self and per-axis gaps', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-align-gap.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: per-side borders render and update', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
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

test('appservice ops: CSS order reorders flow children', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-order.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: overflow hidden clips children to the border box', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-overflow.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
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

test('appservice ops: flex-wrap breaks rows and columns', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-flex-wrap.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-fixed.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: skew and 3D transforms paint', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform3d.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: CSS transitions interpolate before the final frame', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transition.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-animated-style.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: builder updates use fine-grained callbacks', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-builder-update.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: network images share one request per URL', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-netcache-'));
  const bmp = path.join(dir, 'quad.bmp');
  writeQuadBmp(bmp);
  const encoded = fs.readFileSync(bmp).toString('base64');
  const server = spawn(process.execPath, ['-e', `
    const http = require('http');
    const bytes = Buffer.from('${encoded}', 'base64');
    const server = http.createServer((req, res) => {
      res.writeHead(200, {
        'Content-Type': 'image/bmp',
        'Content-Length': String(bytes.length),
      });
      res.end(bytes);
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
    const url = `http://127.0.0.1:${port}/quad.bmp`;
    const ops = path.join(dir, 'two.ops.json');
    fs.writeFileSync(ops, JSON.stringify({
      ops: [
        { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
        { op: 'setStyle', id: 2, value: 'width:320px;height:240px;background:#FFFFFF;' },
        { op: 'createElement', id: 3, tag: 'empty', name: 'image' },
        { op: 'setStyle', id: 3, value: 'width:160px;height:120px;' },
        { op: 'setAttribute', id: 3, name: 'src', value: url },
        { op: 'createElement', id: 4, tag: 'empty', name: 'image' },
        { op: 'setStyle', id: 4, value: 'position:absolute;left:160px;top:0px;width:160px;height:120px;' },
        { op: 'setAttribute', id: 4, name: 'src', value: url },
        { op: 'appendChild', id: 2, child: 3 },
        { op: 'appendChild', id: 2, child: 4 },
        { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
        { op: 'appendChild', id: 1, child: 2 },
        { op: 'attach', root: 1 },
        { op: 'startRender' },
      ],
    }));
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 4000,
      frames: 0,
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    const fetches = (logs.match(/image #\d+: fetch http:\/\//g) || []).length;
    assert.equal(fetches, 1, `expected one fetch, got ${fetches}:\n${logs}`);
    assert.match(logs, /join pending fetch/);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 40, 30), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 280, 30), [0, 255, 0, 255]);
  } finally {
    server.kill();
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: images repeat and cache by URL', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-repeat-'));
  writeQuadBmp(path.join(dir, 'quad.bmp'));
  const ops = path.join(dir, 'repeat.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2, value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'image' },
      { op: 'setStyle', id: 3, value: 'width:32px;height:32px;' },
      { op: 'setAttributes', id: 3, attrs: { src: 'quad.bmp', repeat: 'repeat' } },
      { op: 'createElement', id: 4, tag: 'empty', name: 'image' },
      { op: 'setStyle', id: 4, value: 'width:16px;height:16px;' },
      { op: 'setAttribute', id: 4, name: 'src', value: 'quad.bmp' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'appendChild', id: 2, child: 4 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /cache hit quad\.bmp/);
    assert.ok(pixels, 'no frame was captured');
    // 8x8 tiles: red / green on top, blue / yellow below, repeated.
    assert.deepEqual(pixelAt(pixels, 2, 2), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 6, 2), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 2, 6), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 6, 6), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 18, 2), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 22, 2), [0, 255, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: CSS background-image paints through NewDecoratedBox', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-bg-'));
  writeQuadBmp(path.join(dir, 'quad.bmp'));
  const ops = path.join(dir, 'background.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2, value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'width:160px;height:80px;background-image:url(quad.bmp);' +
          'background-size:cover;background-repeat:no-repeat;' +
          'background-position:center;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /background image #3: quad\.bmp/);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 40, 20), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 120, 20), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 40, 60), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 120, 60), [255, 255, 0, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: background-position and object-position', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-pos-'));
  writeQuadBmp(path.join(dir, 'quad.bmp'));
  const ops = path.join(dir, 'position.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'image' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:0px;top:0px;width:160px;height:80px;' +
          'object-fit:contain;object-position:left top;' },
      { op: 'setAttributes', id: 3, attrs: { src: 'quad.bmp' } },
      { op: 'createElement', id: 4, tag: 'empty', name: 'image' },
      { op: 'setStyle', id: 4,
        value: 'position:absolute;left:0px;top:80px;width:320px;height:80px;' +
          'object-fit:contain;object-position:right bottom;' },
      { op: 'setAttributes', id: 4, attrs: { src: 'quad.bmp' } },
      { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 5,
        value: 'position:absolute;left:0px;top:160px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:contain;' +
          'background-repeat:no-repeat;background-position:center;' },
      { op: 'createElement', id: 6, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 6,
        value: 'position:absolute;left:160px;top:160px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:contain;' +
          'background-repeat:no-repeat;background-position:right bottom;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'appendChild', id: 2, child: 4 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'appendChild', id: 2, child: 6 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');

    // object-position: left top -> 80x80 image at (0,0).
    assert.deepEqual(pixelAt(pixels, 20, 20), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 20), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 20, 60), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 60), [255, 255, 0, 255]);
    // object-position: right bottom -> image at x=240 inside the second box.
    assert.deepEqual(pixelAt(pixels, 250, 100), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 290, 100), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 250, 140), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 290, 140), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 200, 100), [255, 255, 255, 255]);

    // background-position: center -> 80x80 image at x=40.
    assert.deepEqual(pixelAt(pixels, 60, 180), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 100, 180), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 220), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 100, 220), [255, 255, 0, 255]);
    // background-position: right bottom -> image at x=240.
    assert.deepEqual(pixelAt(pixels, 250, 180), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 290, 180), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 200, 180), [255, 255, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: SVG images decode through SvgToRgba', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-svg-'));
  fs.writeFileSync(path.join(dir, 'half.svg'),
    '<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" ' +
    'viewBox="0 0 20 20"><rect width="10" height="20" fill="#FF0000"/>' +
    '<rect x="10" width="10" height="20" fill="#0000FF"/></svg>\n');
  const ops = path.join(dir, 'svg.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2, value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'image' },
      { op: 'setStyle', id: 3, value: 'width:100px;height:100px;' },
      { op: 'setAttribute', id: 3, name: 'src', value: 'half.svg' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 4000,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /SVG half\.svg/);
    assert.ok(pixels, 'no frame was captured');
    assert.deepEqual(pixelAt(pixels, 25, 50), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 75, 50), [0, 0, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: CJK text resolves through the font fallback list', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops);
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

test('appservice ops: editing keys move the cursor and replace selections', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
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

test('appservice ops: recorder samples animated style worklets', {
  skip: prerequisiteSkip() || false,
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

    const { result, logs, pixels } = runHost(recorded.path, {
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

test('appservice ops: composition replaces the selected range', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
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

test('appservice ops: native view forwards pointer events', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-native-view-release.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
    timeoutMs: 2500,
    frames: 0,
    env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
  });
  if (logs.includes('engine artifacts missing')) {
    if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
      assert.fail(`engine is required but not built:\n${logs}`);
    }
    return;
  }
  assert.equal(result.status, 0,
    `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(pixels, 'no frame was captured');
  assert.match(logs, /native view #4: viewId=77 type=canvas/);
  assert.match(logs, /native view dispose: viewId=77/);

  // The update removes the texture; the red page fills its slot again.
  assert.deepEqual(pixelAt(pixels, 60, 60), [255, 0, 0, 255]);
});

test('appservice ops: flex-basis and flex-shrink size items', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-flex-basis.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: isolated inline text wraps at the container width', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-inline-text-wrap.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: swiper current attribute animates to the next page', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-swiper.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: box-sizing content-box adds padding and border', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-box-sizing.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-percent-box.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: position sticky pins the header while scrolling', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-sticky.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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

test('appservice ops: radial and conic gradients paint through NewDecoratedBox', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-radial-conic.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
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

test('appservice ops: background-size lengths scale and tile images', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-bgsize-'));
  writeQuadBmp(path.join(dir, 'quad.bmp'));
  const ops = path.join(dir, 'background-size.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      // 40x20 explicit size at the top-left.
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:0px;top:0px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:40px 20px;' +
          'background-repeat:no-repeat;background-position:left top;' },
      // 50%x50% of the 160x80 box -> 80x40, centred.
      { op: 'createElement', id: 4, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 4,
        value: 'position:absolute;left:160px;top:0px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:50% 50%;' +
          'background-repeat:no-repeat;background-position:center;' },
      // Width 32px, height auto (8x8 image -> 32x32 tile), repeated.
      { op: 'createElement', id: 5, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 5,
        value: 'position:absolute;left:0px;top:80px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:32px auto;' +
          'background-repeat:repeat;' },
      // Height 40px, width auto (-> 40x40), anchored bottom-right.
      { op: 'createElement', id: 6, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 6,
        value: 'position:absolute;left:160px;top:80px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:auto 40px;' +
          'background-repeat:no-repeat;background-position:right bottom;' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'appendChild', id: 2, child: 4 },
      { op: 'appendChild', id: 2, child: 5 },
      { op: 'appendChild', id: 2, child: 6 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 1800,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');

    // 40x20 at (0,0).
    assert.deepEqual(pixelAt(pixels, 10, 5), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 30, 5), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 10, 15), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 30, 15), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 50, 5), [255, 255, 255, 255]);

    // 80x40 centred in the second box (x 200..280, y 20..60).
    assert.deepEqual(pixelAt(pixels, 220, 30), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 260, 30), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 220, 50), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 260, 50), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 180, 10), [255, 255, 255, 255]);

    // 32x32 tiles: the second tile starts at x=32 and the second row at y=112.
    assert.deepEqual(pixelAt(pixels, 5, 85), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 20, 85), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 5, 100), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 37, 85), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 52, 85), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 5, 117), [255, 0, 0, 255]);

    // 40x40 at the bottom-right corner (x 280..320, y 120..160).
    assert.deepEqual(pixelAt(pixels, 290, 130), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 310, 130), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 290, 150), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 310, 150), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 200, 100), [255, 255, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: background-size updates repaint images and gradients', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-bgsize-up-'));
  writeQuadBmp(path.join(dir, 'quad.bmp'));
  const ops = path.join(dir, 'background-size-update.ops.json');
  fs.writeFileSync(ops, JSON.stringify({
    ops: [
      { op: 'createElement', id: 2, tag: 'empty', name: 'page' },
      { op: 'setStyle', id: 2,
        value: 'width:320px;height:240px;background:#FFFFFF;' },
      { op: 'createElement', id: 3, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 3,
        value: 'position:absolute;left:0px;top:0px;width:160px;height:80px;' +
          'background-image:url(quad.bmp);background-size:40px 20px;' +
          'background-repeat:no-repeat;background-position:left top;' },
      { op: 'createElement', id: 4, tag: 'empty', name: 'view' },
      { op: 'setStyle', id: 4,
        value: 'position:absolute;left:0px;top:80px;width:100px;height:100px;' +
          'background:linear-gradient(90deg,#FF0000,#0000FF);' },
      { op: 'appendChild', id: 2, child: 3 },
      { op: 'appendChild', id: 2, child: 4 },
      { op: 'createElement', id: 1, tag: 'empty', name: 'body' },
      { op: 'appendChild', id: 1, child: 2 },
      { op: 'attach', root: 1 },
      { op: 'startRender' },
    ],
    updates: [
      { afterMs: 300, ops: [
        { op: 'setStyle', id: 3,
          value: 'background-size:80px 40px;background-position:center;' },
        { op: 'setStyle', id: 4,
          value: 'background-size:50px 50px;background-repeat:no-repeat;' +
            'background-position:center;' },
      ] },
    ],
  }));
  try {
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 2600,
      frames: 0,
      env: { SKYLINE_RESOURCE_ROOT: dir },
    });
    assert.equal(result.status, 0,
      `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.ok(pixels, 'no frame was captured');

    // The image grew to 80x40 and moved to the box centre (x 40..120, y 20..60).
    assert.deepEqual(pixelAt(pixels, 60, 30), [255, 0, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 100, 30), [0, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 60, 50), [0, 0, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 100, 50), [255, 255, 0, 255]);
    assert.deepEqual(pixelAt(pixels, 20, 10), [255, 255, 255, 255]);

    // The gradient shrank to a centred 50x50 tile (box at y 80..180).
    const gradientLeft = pixelAt(pixels, 30, 130);
    const gradientRight = pixelAt(pixels, 70, 130);
    assert.ok(gradientLeft[0] > 200 && gradientLeft[2] < 55,
      `centred gradient left edge should be red, got ${gradientLeft}`);
    assert.ok(gradientRight[2] > 200 && gradientRight[0] < 55,
      `centred gradient right edge should be blue, got ${gradientRight}`);
    assert.deepEqual(pixelAt(pixels, 20, 130), [255, 255, 255, 255]);
    assert.deepEqual(pixelAt(pixels, 80, 130), [255, 255, 255, 255]);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('appservice ops: transform-origin z moves the 3D pivot', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const fixture = path.join(
    repoRoot, 'test', 'fixtures', 'appservice-transform-origin-z.ops.json');
  const { result, logs, pixels } = runHost(fixture, {
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
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

test('appservice ops: custom route attach configures CreatePageRoute', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
      timeoutMs: 3500,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: share-element builds a SkylineHero with its WXML props', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs } = runHost(ops, {
      timeoutMs: 3000,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

test('appservice ops: rebuilding the top page keeps covered route images', {
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(fixture, {
      timeoutMs: 2200,
      frames: 0,
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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
  skip: prerequisiteSkip() || false,
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
    const { result, logs, pixels } = runHost(ops, {
      timeoutMs: 2600,
      frames: 0,
      env: { SKYLINE_APP_SERVICE_DEBUG: '1' },
      args: ['--pointer-script', script],
    });
    if (logs.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${logs}`);
      }
      return;
    }
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

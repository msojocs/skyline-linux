'use strict';

// Phase-3 appservice/V8 operation replay.
//
// `tools/record-skyline-ops.js` runs the official Skyline renderer bundle
// (exparser + skyline-js) in Node's V8 and captures the node-operation stream
// that the bundle would send to `skyline.node`'s native bindings.  The embedded
// runtime (native/runtime/skyline_runtime_stub.cc) replays that stream through
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
    '--shm-key', key,
    '--timeout-ms', String(options.timeoutMs || 20000),
    ...(options.frames === undefined ? [] : ['--frames', String(options.frames)]),
    ...(options.args || []),
    '--no-shutdown',
    '--entrypoint-arg', '1',
    '--entrypoint-arg', '1',
    '--entrypoint-arg', String(width),
    '--entrypoint-arg', String(height),
    '--entrypoint-arg', '1',
    '--entrypoint-arg', 'stub',
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
      assert.equal(data.readUInt32LE(4), frameSize);
      pixels = data.subarray(8, 8 + frameSize);
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
  // official appservice guest (docs/feasibility.md section 3).
  assert.equal(
    crypto.createHash('sha256').update(pixels).digest('hex'),
    'd79f71ad5f40824a1395d8d1cf3890525daa949e4a96317841c7c2e466e23f76',
  );
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
  assert.ok(details.presentCount >= 3, `expected repaints, presentCount=${details.presentCount}`);
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
    // The fake bytes are not a valid font, so Dart reports FontFaceLoadStatus.error
    // (2) back through NotifyFontFaceLoad after the resource channel completed.
    assert.match(logs, /NotifyFontFaceLoad: id=\d+ status=2/);
  } finally {
    try {
      fs.rmSync(resourceRoot, { recursive: true, force: true });
    } catch {
      // Best effort.
    }
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
    // ShouldGestureResponse) and feeds the scroll position.
    assert.match(logs, /ShouldGestureResponse: 1 -> 1/);
    assert.match(logs, /ShouldAcceptGesture: 1 -> 1/);
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

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
          'background-size:cover;background-repeat:no-repeat;' },
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

'use strict';

// Phase-3 Skyline page smoke test: the embedded runtime (statically linked into
// libflutter_engine.so) builds a real page through the official package:skyline
// constructors:
//
//   CreatePageRoute -> root YGNode -> YGConnector -> 3 child YGNodes
//   (DecoratedBox #FF3366 / #33CC66 / #3366FF, vertical stripes)
//
// The page is laid out by `YogaCalculateLayout`, painted by Flutter, and
// captured from /dev/shm.  Requires `bash
// native/engine/patch-skyline-runtime.sh` (which stages `libskyline.so` next to
// the rebuilt engine).
//
// Skips when the engine/host/kernel or embedded runtime is missing.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { devtoolsEntrypointArgs } = require('../../test/lib/host');

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

const width = 320;
const height = 240;
const frameSize = width * height * 4;
const stripeColors = [
  [255, 51, 102, 255],  // 0xFFFF3366
  [51, 204, 102, 255],  // 0xFF33CC66
  [51, 102, 255, 255],  // 0xFF3366FF
];

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

test('skyline page: embedded runtime renders YGNode + DecoratedBox', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_page_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: '1',
  };

  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '20000',
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');

    if (diagnostics.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${diagnostics}`);
      }
      return;
    }

    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.equal(details.width, width);
    assert.equal(details.height, height);
    assert.equal(details.frameSize, frameSize);
    assert.ok(details.presentCount >= 1, 'no frame was presented');

    // The embedded runtime used the full Dart API and reached the real page
    // constructors.
    assert.match(result.stderr, /embedded Dart API ready/);
    assert.match(result.stderr, /NotifyFlutterReady -> RunApplication/);
    assert.match(result.stderr, /demo page -> CreatePageRoute/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    const pixelAt = y => {
      const offset = (y * width + Math.floor(width / 2)) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    };
    const stripeHeight = Math.floor(height / 3);
    assert.deepEqual(pixelAt(0), stripeColors[0], 'top stripe must be #FF3366');
    assert.deepEqual(pixelAt(stripeHeight - 1), stripeColors[0], 'top stripe end');
    assert.deepEqual(pixelAt(stripeHeight), stripeColors[1], 'middle stripe must be #33CC66');
    assert.deepEqual(pixelAt(2 * stripeHeight - 1), stripeColors[1], 'middle stripe end');
    assert.deepEqual(pixelAt(2 * stripeHeight), stripeColors[2], 'bottom stripe must be #3366FF');
    assert.deepEqual(pixelAt(height - 1), stripeColors[2], 'bottom stripe end');
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

test('skyline page: text node renders through the Flutter text engine', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_text_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: 'text',
  };

  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '20000',
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');

    if (diagnostics.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${diagnostics}`);
      }
      return;
    }

    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.equal(details.width, width);
    assert.equal(details.height, height);
    assert.match(result.stderr, /text page -> CreatePageRoute/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);

    // White page with black glyphs inside the 20px inset.
    assert.deepEqual([...pixels.subarray(0, 4)], [255, 255, 255, 255], 'page background must be white');
    let dark = 0;
    let minX = width;
    let minY = height;
    let maxX = 0;
    let maxY = 0;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const offset = (y * width + x) * 4;
        const [r, g, b] = [pixels[offset], pixels[offset + 1], pixels[offset + 2]];
        if (r < 100 && g < 100 && b < 100) {
          dark++;
          if (x < minX) minX = x;
          if (y < minY) minY = y;
          if (x > maxX) maxX = x;
          if (y > maxY) maxY = y;
        }
      }
    }
    assert.ok(dark > 300, `expected rendered glyphs, found ${dark} dark pixels`);
    assert.ok(minX >= 20 && minY >= 20, `text bbox starts at (${minX},${minY}), outside the inset`);
    assert.ok(maxX - minX > 100, `text bbox width ${maxX - minX} looks too small`);
    assert.ok(maxY < height - 20, `text bbox ends at ${maxY}, outside the inset`);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

test('skyline page: DecoratedBox border/radius and text render as a card', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_card_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: 'card',
  };

  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '20000',
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');

    if (diagnostics.includes('engine artifacts missing')) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${diagnostics}`);
      }
      return;
    }

    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.match(result.stderr, /card page -> CreatePageRoute/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    const pixelAt = (x, y) => {
      const offset = (y * width + x) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    };

    // Card spans (40,40)-(280,200), 3px solid #3366FF border, #F5F5F5 fill.
    assert.deepEqual(pixelAt(0, 0), [255, 255, 255, 255], 'page background');
    assert.deepEqual(pixelAt(160, 41), [51, 102, 255, 255], 'top border');
    assert.deepEqual(pixelAt(160, 80), [245, 245, 245, 255], 'card fill');
    assert.deepEqual(pixelAt(160, 199), [51, 102, 255, 255], 'bottom border');
    assert.deepEqual(pixelAt(300, 120), [255, 255, 255, 255], 'outside the card');

    let dark = 0;
    for (let i = 0; i < pixels.length; i += 4) {
      if (pixels[i] < 100 && pixels[i + 1] < 100 && pixels[i + 2] < 100) dark++;
    }
    assert.ok(dark > 300, `expected card text, found ${dark} dark pixels`);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

test('skyline page: JSON descriptor drives row flex layout + text', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_json_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const descriptor = JSON.stringify({
    direction: 'row',
    padding: 10,
    gap: 10,
    children: [
      { type: 'view', color: '#FF3366', flex: 1 },
      { type: 'view', color: '#33CC66', flex: 2 },
      { type: 'text', text: 'Skyline', fontSize: 24, color: '#000000', size: 90 },
    ],
  });
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: descriptor,
  };

  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '20000',
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.match(result.stderr, /json page -> CreatePageRoute/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    const pixelAt = (x, y) => {
      const offset = (y * width + x) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    };

    // Row: padding 10, gap 10, flex 1:2, fixed 90px text at the end.
    assert.deepEqual(pixelAt(0, 120), [255, 255, 255, 255], 'left padding');
    assert.deepEqual(pixelAt(15, 120), [255, 51, 102, 255], 'first flex child');
    assert.deepEqual(pixelAt(70, 120), [255, 51, 102, 255], 'first flex child end');
    assert.deepEqual(pixelAt(78, 120), [255, 255, 255, 255], 'gap between flex children');
    assert.deepEqual(pixelAt(90, 120), [51, 204, 102, 255], 'second flex child');
    assert.deepEqual(pixelAt(200, 120), [51, 204, 102, 255], 'second flex child end');
    assert.deepEqual(pixelAt(225, 120), [255, 255, 255, 255], 'text child background');
    assert.deepEqual(pixelAt(315, 120), [255, 255, 255, 255], 'right padding');

    let dark = 0;
    for (let i = 0; i < pixels.length; i += 4) {
      if (pixels[i] < 100 && pixels[i + 1] < 100 && pixels[i + 2] < 100) dark++;
    }
    assert.ok(dark > 100, `expected text in the row, found ${dark} dark pixels`);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

test('skyline page: nested JSON tree lays out row inside column', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_nested_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const descriptor = JSON.stringify({
    direction: 'column',
    padding: 10,
    gap: 10,
    children: [
      {
        type: 'view',
        color: '#FF3366',
        flex: 1,
        padding: 8,
        gap: 8,
        direction: 'row',
        children: [
          { type: 'view', color: '#33CC66', flex: 1 },
          { type: 'text', text: 'Hi', fontSize: 20, color: '#FFFFFF', size: 60 },
        ],
      },
      { type: 'view', color: '#3366FF', size: 60 },
    ],
  });
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: descriptor,
  };

  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '20000',
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.match(result.stderr, /json page -> CreatePageRoute/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    const pixelAt = (x, y) => {
      const offset = (y * width + x) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    };

    // Outer column: pink flex child (150px) + 10px gap + blue 60px child.
    assert.deepEqual(pixelAt(0, 0), [255, 255, 255, 255], 'page padding');
    assert.deepEqual(pixelAt(15, 20), [255, 51, 102, 255], 'pink node padding');
    // Pink child (10,10)-(309,159), 8px padding, row: green flex 1 + 60px text.
    assert.deepEqual(pixelAt(20, 20), [51, 204, 102, 255], 'nested green child');
    assert.deepEqual(pixelAt(100, 100), [51, 204, 102, 255], 'nested green child body');
    assert.deepEqual(pixelAt(240, 20), [255, 51, 102, 255], 'text area background');
    assert.deepEqual(pixelAt(160, 165), [255, 255, 255, 255], 'gap between outer children');
    assert.deepEqual(pixelAt(20, 175), [51, 102, 255, 255], 'blue child');
    assert.deepEqual(pixelAt(315, 120), [255, 255, 255, 255], 'right padding');

    let white = 0;
    for (let y = 18; y < 152; y++) {
      for (let x = 242; x < 302; x++) {
        const offset = (y * width + x) * 4;
        if (pixels[offset] > 200 && pixels[offset + 1] > 200 && pixels[offset + 2] > 200) {
          white++;
        }
      }
    }
    assert.ok(white > 50, `expected white glyphs in the nested text area, found ${white}`);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

function runJsonPage(descriptor, keyPrefix) {
  const key = `${keyPrefix}_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: JSON.stringify(descriptor),
  };
  const result = spawnSync(host, [
    '--assets', assets,
    '--icu', icu,
    '--width', String(width),
    '--height', String(height),
    '--shm-key', key,
    '--timeout-ms', '20000',
    '--no-shutdown',
    ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
  ], { encoding: 'utf8', timeout: 45000, env });

  const details = parseResult(result.stdout);
  const diagnostics = [
    result.stdout && result.stdout.trim(),
    result.stderr && result.stderr.trim(),
  ].filter(Boolean).join('\n');
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
  assert.match(result.stderr, /json page -> CreatePageRoute/);
  const buffer = fs.readFileSync(shmPath);
  const dataSize = Number(buffer.readBigUInt64LE(0));
  assert.equal(buffer.length, 16 + dataSize);
  const data = buffer.subarray(16);
  assert.equal(data.readUInt32LE(4), frameSize);
  const pixels = data.subarray(8, 8 + frameSize);
  return {
    pixelAt(x, y) {
      const offset = (y * width + x) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    },
    cleanup() {
      try {
        fs.unlinkSync(shmPath);
      } catch {
        // Missing file is fine.
      }
    },
  };
}

test('skyline page: opacity node composites children over the page', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runJsonPage({
    padding: 20,
    children: [
      { type: 'opacity', opacity: 0.5, flex: 1, children: [
        { type: 'view', color: '#FF0000', flex: 1 },
      ] },
    ],
  }, 'skyline_opacity');
  try {
    const center = page.pixelAt(160, 120);
    // 50% red over white; allow +/-1 for raster rounding.
    assert.ok(Math.abs(center[0] - 255) <= 1, `red ${center[0]}`);
    assert.ok(Math.abs(center[1] - 127) <= 1, `green ${center[1]}`);
    assert.ok(Math.abs(center[2] - 127) <= 1, `blue ${center[2]}`);
    assert.equal(center[3], 255);
    assert.deepEqual(page.pixelAt(0, 0), [255, 255, 255, 255], 'page padding stays white');
  } finally {
    page.cleanup();
  }
});

test('skyline page: clip node rounds the corners of its children', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runJsonPage({
    padding: 40,
    children: [
      { type: 'clip', radius: 40, flex: 1, children: [
        { type: 'view', color: '#FF3333', flex: 1 },
      ] },
    ],
  }, 'skyline_clip');
  try {
    assert.deepEqual(page.pixelAt(41, 41), [255, 255, 255, 255], 'top-left corner is clipped');
    assert.deepEqual(page.pixelAt(50, 50), [255, 255, 255, 255], 'inside the corner arc');
    assert.deepEqual(page.pixelAt(70, 70), [255, 51, 51, 255], 'inside the clip');
    assert.deepEqual(page.pixelAt(160, 120), [255, 51, 51, 255], 'center');
    assert.deepEqual(page.pixelAt(279, 199), [255, 255, 255, 255], 'bottom-right corner is clipped');
  } finally {
    page.cleanup();
  }
});

test('skyline page: linear gradient fills a node', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runJsonPage({
    children: [
      { type: 'view', flex: 1, gradient: { angle: 0, colors: ['#FF0000', '#0000FF'] } },
    ],
  }, 'skyline_gradient');
  try {
    const top = page.pixelAt(160, 0);
    const center = page.pixelAt(160, 120);
    const bottom = page.pixelAt(160, 239);
    assert.ok(top[2] > 200 && top[0] < 20, `top should be blue: ${top}`);
    assert.ok(bottom[0] > 235 && bottom[2] < 20, `bottom should be red: ${bottom}`);
    assert.ok(center[0] > 100 && center[0] < 160, `center red channel: ${center}`);
    assert.ok(center[2] > 100 && center[2] < 160, `center blue channel: ${center}`);
    assert.ok(center[1] < 10, `center green stays low: ${center}`);
  } finally {
    page.cleanup();
  }
});

test('skyline page: box shadow paints outside the node', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runJsonPage({
    padding: 40,
    children: [
      {
        type: 'view',
        color: '#FF3366',
        flex: 1,
        shadow: { color: '#80000000', offsetY: 10, blur: 10 },
      },
    ],
  }, 'skyline_shadow');
  try {
    assert.deepEqual(page.pixelAt(160, 120), [255, 51, 102, 255], 'box fill');
    const below = page.pixelAt(160, 203);
    assert.ok(below[0] < 200, `shadow below the box: ${below}`);
    assert.ok(Math.abs(below[0] - below[1]) <= 2 && Math.abs(below[1] - below[2]) <= 2,
      `shadow should be gray: ${below}`);
    const edge = page.pixelAt(39, 120);
    assert.ok(edge[0] < 230, `shadow at the left edge: ${edge}`);
    const outside = page.pixelAt(160, 230);
    assert.ok(outside[0] > 245, `shadow fades out: ${outside}`);
  } finally {
    page.cleanup();
  }
});

test('skyline page: text style and alignment options apply', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runJsonPage({
    children: [
      {
        type: 'text',
        text: 'Skyline',
        fontSize: 32,
        fontWeight: 6,
        textAlign: 2,
        flex: 1,
      },
    ],
  }, 'skyline_textstyle');
  try {
    const pixels = [];
    // Re-read the frame through the helper's closure by sampling every pixel.
    let dark = 0;
    let minX = width;
    let maxX = 0;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const p = page.pixelAt(x, y);
        if (p[0] < 100 && p[1] < 100 && p[2] < 100) {
          dark++;
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
        }
      }
    }
    assert.ok(dark > 300, `bold text should render, found ${dark} dark pixels`);
    const centerX = (minX + maxX) / 2;
    assert.ok(Math.abs(centerX - width / 2) < 20, `text center ${centerX} should be near ${width / 2}`);
  } finally {
    page.cleanup();
  }
});

test('skyline page: native delayed update repaints through DecoratedBoxUpdateColor', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_update_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: JSON.stringify({
      children: [
        {
          type: 'view',
          color: '#FF0000',
          updateColor: '#0000FF',
          updateAfterMs: 300,
          flex: 1,
        },
      ],
    }),
  };
  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '1500',
      '--frames', '0',  // capture until timeout, keep the last frame
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });
    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.match(result.stderr, /color update in 300 ms/);
    assert.ok(details.presentCount >= 2, `expected a repaint, presentCount=${details.presentCount}`);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    const center = (Math.floor(height / 2) * width + Math.floor(width / 2)) * 4;
    assert.deepEqual([...pixels.subarray(center, center + 4)], [0, 0, 255, 255],
      'the last frame must reflect the delayed color update');
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

test('skyline page: appendChild/removeChild ops relayout the tree', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_ops_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: JSON.stringify({
      children: [{ type: 'view', color: '#FF0000', flex: 1 }],
      ops: [
        { afterMs: 100, op: 'append', child: { type: 'view', color: '#00FF00', flex: 1 } },
        { afterMs: 300, op: 'remove', index: 0 },
      ],
    }),
  };
  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--shm-key', key,
      '--timeout-ms', '2500',
      '--frames', '0',
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], { encoding: 'utf8', timeout: 45000, env });
    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.match(result.stderr, /append child in 100 ms/);
    assert.match(result.stderr, /remove child in 300 ms/);
    assert.ok(details.presentCount >= 3, `expected relayout frames, presentCount=${details.presentCount}`);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    const pixelAt = (x, y) => {
      const offset = (y * width + x) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    };
    // Red was removed; the appended green child flexes to the whole page.
    for (const [x, y] of [[0, 0], [160, 40], [160, 120], [160, 200], [319, 239]]) {
      assert.deepEqual(pixelAt(x, y), [0, 255, 0, 255],
        `expected green after append/remove at (${x},${y})`);
    }
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

function runUpdatePage(descriptor, keyPrefix) {
  const key = `${keyPrefix}_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_STUB_PAGE: JSON.stringify(descriptor),
  };
  const result = spawnSync(host, [
    '--assets', assets,
    '--icu', icu,
    '--width', String(width),
    '--height', String(height),
    '--shm-key', key,
    '--timeout-ms', '2500',
    '--frames', '0',
    '--no-shutdown',
    ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
  ], { encoding: 'utf8', timeout: 45000, env });
  const details = parseResult(result.stdout);
  const diagnostics = [
    result.stdout && result.stdout.trim(),
    result.stderr && result.stderr.trim(),
  ].filter(Boolean).join('\n');
  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
  const buffer = fs.readFileSync(shmPath);
  const dataSize = Number(buffer.readBigUInt64LE(0));
  assert.equal(buffer.length, 16 + dataSize);
  const data = buffer.subarray(16);
  assert.equal(data.readUInt32LE(4), frameSize);
  const pixels = data.subarray(8, 8 + frameSize);
  return {
    pixelAt(x, y) {
      const offset = (y * width + x) * 4;
      return [...pixels.subarray(offset, offset + 4)];
    },
    cleanup() {
      try {
        fs.unlinkSync(shmPath);
      } catch {
        // Missing file is fine.
      }
    },
  };
}

test('skyline page: opacity update applies at runtime', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runUpdatePage({
    children: [
      {
        type: 'opacity',
        opacity: 1.0,
        updateOpacity: { value: 0.5, afterMs: 300 },
        flex: 1,
        children: [{ type: 'view', color: '#FF0000', flex: 1 }],
      },
    ],
  }, 'skyline_opacity_update');
  try {
    const center = page.pixelAt(160, 120);
    assert.ok(Math.abs(center[0] - 255) <= 1, `red ${center[0]}`);
    assert.ok(Math.abs(center[1] - 127) <= 1, `green ${center[1]}`);
    assert.ok(Math.abs(center[2] - 127) <= 1, `blue ${center[2]}`);
  } finally {
    page.cleanup();
  }
});

test('skyline page: border update applies at runtime', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const page = runUpdatePage({
    padding: 20,
    children: [
      {
        type: 'view',
        color: '#FF3366',
        updateBorder: { width: 8, color: '#FFFF00', afterMs: 300 },
        flex: 1,
      },
    ],
  }, 'skyline_border_update');
  try {
    assert.deepEqual(page.pixelAt(160, 21), [255, 255, 0, 255], 'updated yellow border');
    assert.deepEqual(page.pixelAt(160, 120), [255, 51, 102, 255], 'fill stays pink');
  } finally {
    page.cleanup();
  }
});

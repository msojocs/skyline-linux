'use strict';

// Appservice op replay: Images, resource loading and the HTTP channel.
// Shared setup and helpers: ./helpers.js.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const {
  checkOpsBuild,
  opsSkip,
  pixelAt,
  repoRoot,
  runOps,
  writeQuadBmp,
} = require('./helpers');

checkOpsBuild('appservice ops (image)');

test('appservice ops: image nodes paint through NewDirectRenderWidget', {
  skip: opsSkip,
  timeout: 60000,
}, () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-image.ops.json');
  const resourceRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-img-'));
  writeQuadBmp(path.join(resourceRoot, 'quad.bmp'));
  try {
    const { result, logs, pixels } = runOps(fixture, {
      env: { SKYLINE_RESOURCE_ROOT: resourceRoot, SKYLINE_APP_SERVICE_DEBUG: '1' },
    });
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
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(fixture, {
      env: { SKYLINE_RESOURCE_ROOT: resourceRoot },
    });
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

test('appservice ops: ForwardHttpGet bridges to RequestHttpResource', {
  skip: opsSkip,
  timeout: 60000,
}, async () => {
  const fixture = path.join(repoRoot, 'test', 'fixtures', 'appservice-stripes.ops.json');
  // The server must live in a separate process: runOps() blocks this event
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

    const { result, logs } = runOps(fixture, {
      timeoutMs: 2500,
      frames: 0,
      env: {
        SKYLINE_HTTP_TEST_URL: `http://127.0.0.1:${port}/`,
        SKYLINE_APP_SERVICE_DEBUG: '1',
      },
    });
    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
    assert.match(logs, /ForwardHttpGet: id=\d+ url=http:\/\/127\.0\.0\.1:\d+\//);
    assert.match(logs, /SKYLINE_HTTP_RESULT 15 skyline-http-ok/);
  } finally {
    server.kill();
  }
});

test('appservice ops: image objectFit contains the bitmap', {
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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
  skip: opsSkip,
  timeout: 60000,
}, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-net-'));
  const bmp = path.join(dir, 'quad.bmp');
  writeQuadBmp(bmp);
  const encoded = fs.readFileSync(bmp).toString('base64');
  // The server must live in a separate process: runOps() blocks this event
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
    const { result, logs, pixels } = runOps(ops, {
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

test('appservice ops: network images share one request per URL', {
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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

test('appservice ops: background-size lengths scale and tile images', {
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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
  skip: opsSkip,
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
    const { result, logs, pixels } = runOps(ops, {
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

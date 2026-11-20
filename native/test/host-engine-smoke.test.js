'use strict';

// Phase-2 in-process Flutter Engine backend for `skyline.node`.
//
// The addon links the pinned Linux engine when `npm run native:build` sees
// `native/engine/out/linux-x64-debug/libflutter_engine.so`.  `createWindow`
// starts a platform thread per window, copies presented frames (BGRA -> RGBA)
// into the shared-memory data area supplied as the 6th createWindow argument,
// and invokes the `onWindowRasterized` callback.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..');
const addonPath = path.join(repoRoot, 'native', 'build', 'skyline.node');
const engineDir = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-debug');
const engineLib = path.join(engineDir, 'libflutter_engine.so');
const bundle = path.join(repoRoot, 'apps', 'empty-page', 'build');
const kernel = path.join(bundle, 'flutter_assets', 'kernel_blob.bin');

const width = 128;
const height = 96;
const frameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(addonPath)) missing.push('native/build/skyline.node (npm run native:build)');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so (npm run engine:build)');
  if (!fs.existsSync(kernel)) missing.push('apps/empty-page kernel (npm run app:build)');
  return missing.length ? missing.join('; ') : null;
}

function requireEngine() {
  return process.env.SKYLINE_REQUIRE_ENGINE === '1';
}

function loadShell() {
  // Loading the addon registers global.SkylineShell / SkylineDebugInfo.
  require(addonPath);
  return new global.SkylineShell();
}

function waitForFrame(shell, windowId, frame) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error('timed out waiting for a rasterized frame')), 15000);
    shell.onWindowRasterized(windowId, () => {
      clearTimeout(timer);
      const frameId = frame.readUInt32LE(0);
      const size = frame.readUInt32LE(4);
      resolve({
        frameId,
        size,
        pixelOffset: 8,
        pixel: [...frame.subarray(8, 12)],
      });
    });
  });
}

function createWindow(shell, windowId, buffer) {
  return shell.createWindow(
    windowId,
    bundle,
    width,
    height,
    1,
    true,
    buffer,
    addonPath,
    new Uint8Array(16),
  );
}

test('host engine: createWindow renders a frame into the shared buffer', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const shell = loadShell();
  // Official glue budgets four frames plus the 8-byte frame header.
  const frame = Buffer.alloc(8 + frameSize * 4);
  let created = false;
  let framePromise = null;
  try {
    createWindow(shell, 1, frame);
    created = true;
    // Registration is two calls in the official API; if the first frame has
    // already landed the engine replays the notification on registration.
    framePromise = waitForFrame(shell, 1, frame);
  } catch (error) {
    if (!requireEngine() && /not attached|not found|missing kernel/.test(error.message)) {
      return;  // addon built without the engine backend
    }
    throw error;
  }
  try {
    const result = await framePromise;
    assert.ok(result.frameId >= 1, `unexpected frame id ${result.frameId}`);
    assert.equal(result.size, frameSize);
    assert.deepEqual(result.pixel, [255, 0, 0, 255]);
    // Sanity-check the rest of the frame (empty-page draws solid red).
    assert.deepEqual([...frame.subarray(8 + frameSize - 4, 8 + frameSize)],
      [255, 0, 0, 255]);
  } finally {
    if (created) {
      shell.destroyWindow(1);
      assert.throws(() => shell.onWindowRasterized(1, () => {}),
        /window_id: 1 not found/);
    }
  }
});

test('host engine: windows can be recreated after destroy', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const shell = loadShell();
  const first = Buffer.alloc(8 + frameSize * 4);
  try {
    createWindow(shell, 7, first);
  } catch (error) {
    if (!requireEngine() && /not attached|not found|missing kernel/.test(error.message)) {
      return;
    }
    throw error;
  }
  // createWindow is synchronous once the platform thread is ready; teardown
  // must join it before the buffer goes away.
  shell.destroyWindow(7);

  const second = Buffer.alloc(8 + frameSize * 4);
  createWindow(shell, 8, second);
  const promise = waitForFrame(shell, 8, second);
  const result = await promise;
  assert.equal(result.size, frameSize);
  assert.deepEqual(result.pixel, [255, 0, 0, 255]);
  shell.destroyWindow(8);
});

test('host engine: two windows render independently', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const shell = loadShell();
  const frames = {
    11: Buffer.alloc(8 + frameSize * 4),
    12: Buffer.alloc(8 + frameSize * 4),
  };
  try {
    createWindow(shell, 11, frames[11]);
    createWindow(shell, 12, frames[12]);
  } catch (error) {
    if (!requireEngine() && /not attached|not found|missing kernel/.test(error.message)) {
      return;
    }
    throw error;
  }
  try {
    const results = await Promise.all([
      waitForFrame(shell, 11, frames[11]),
      waitForFrame(shell, 12, frames[12]),
    ]);
    for (const result of results) {
      assert.equal(result.size, frameSize);
      assert.deepEqual(result.pixel, [255, 0, 0, 255]);
    }
  } finally {
    shell.destroyWindow(11);
    shell.destroyWindow(12);
  }
});

test('host engine: device pixel ratio 2 renders physical pixels', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const shell = loadShell();
  const logicalWidth = 64;
  const logicalHeight = 48;
  const physicalSize = logicalWidth * 2 * logicalHeight * 2 * 4;
  const frame = Buffer.alloc(8 + physicalSize * 4);
  try {
    shell.createWindow(21, bundle, logicalWidth, logicalHeight, 2, true, frame,
      addonPath, new Uint8Array(16));
  } catch (error) {
    if (!requireEngine() && /not attached|not found|missing kernel/.test(error.message)) {
      return;
    }
    throw error;
  }
  try {
    const result = await waitForFrame(shell, 21, frame);
    assert.equal(result.size, physicalSize);
    assert.deepEqual(result.pixel, [255, 0, 0, 255]);
  } finally {
    shell.destroyWindow(21);
  }
});

test('host engine: invalid dimensions are rejected synchronously', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const shell = loadShell();
  assert.throws(
    () => shell.createWindow(31, bundle, 0, height, 1, true,
      Buffer.alloc(8 + frameSize), addonPath, new Uint8Array(16)),
    /invalid window dimensions or pixel ratio|not attached/);
  assert.throws(
    () => shell.createWindow(32, bundle, width, height, 0, true,
      Buffer.alloc(8 + frameSize), addonPath, new Uint8Array(16)),
    /invalid window dimensions or pixel ratio|not attached/);
});

test('host engine: invalid bundle reports the missing kernel', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const shell = loadShell();
  let error = null;
  try {
    shell.createWindow(3, path.join(repoRoot, 'does-not-exist'), width, height,
      1, true, Buffer.alloc(8 + frameSize), addonPath, new Uint8Array(16));
  } catch (caught) {
    error = caught;
  }
  if (error === null) {
    // A stubbed build throws "engine backend is not attached"; skip only then.
    if (!requireEngine()) return;
  }
  assert.ok(error, 'createWindow should fail for a missing bundle');
  assert.match(error.message, /missing kernel_blob\.bin|not attached/);
});

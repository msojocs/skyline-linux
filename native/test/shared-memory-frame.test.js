'use strict';

// Phase-2/4 bridge: `skyline.node` writes frames into the Linux
// `skyline-shared-memory` data area.
//
// `sharedMemory.setMemory(key, bytes)` returns a live external ArrayBuffer over
// the data region (frame header + pixels).  `createWindow` accepts it as the
// 6th argument; the engine backend writes frameId/frameSize/RGBA into it and
// `getMemory(key)` observes the same bytes.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..');
const addonPath = path.join(repoRoot, 'native', 'build', 'skyline.node');
const engineLib = path.join(
  repoRoot, 'native', 'engine', 'out', 'linux-x64-debug', 'libflutter_engine.so',
);
const bundle = path.join(repoRoot, 'apps', 'empty-page', 'build');
const kernel = path.join(bundle, 'flutter_assets', 'kernel_blob.bin');

const width = 128;
const height = 96;
const frameSize = width * height * 4;

function sharedMemoryPath() {
  const candidates = [
    process.env.SKYLINE_LINUX_SHARED_MEMORY,
    process.env.SKYLINE_SHARED_MEMORY_MODULE,
    path.resolve(repoRoot, '..', 'skyline-shared-memory', 'build',
      'Release', 'sharedMemory.node'),
    path.resolve(repoRoot, '..', 'skyline-shared-memory', 'build',
      'sharedMemory.node'),
  ].filter(Boolean);
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(addonPath)) missing.push('native/build/skyline.node');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so');
  if (!fs.existsSync(kernel)) missing.push('apps/empty-page kernel (npm run app:build)');
  const shared = sharedMemoryPath();
  if (!shared) missing.push('skyline-shared-memory/sharedMemory.node');
  return missing.length ? missing.join('; ') : null;
}

test('shared memory: engine frames land in the sharedMemory data area', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const shared = require(sharedMemoryPath());
  require(addonPath);
  const shell = new global.SkylineShell();
  const key = `skyline_frame_${process.pid}_${Date.now().toString(36)}`;
  // Official glue budgets four RGBA frames plus the 8-byte frame header.
  const bytes = 8 + frameSize * 4;
  const buffer = shared.setMemory(key, bytes);
  assert.ok(buffer, 'setMemory should return the data area');

  let created = false;
  try {
    shell.createWindow(1, bundle, width, height, 1, true, buffer, addonPath,
      new Uint8Array(16));
    created = true;
    const frame = await new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('timed out waiting for a frame')), 15000);
      shell.onWindowRasterized(1, () => {
        clearTimeout(timer);
        resolve(Buffer.from(shared.getMemory(key)));
      });
    });
    assert.equal(frame.readUInt32LE(4), frameSize);
    assert.ok(frame.readUInt32LE(0) >= 1);
    assert.deepEqual([...frame.subarray(8, 12)], [255, 0, 0, 255]);
    assert.deepEqual(
      [...frame.subarray(8 + frameSize - 4, 8 + frameSize)],
      [255, 0, 0, 255]);
  } finally {
    if (created) shell.destroyWindow(1);
    if (typeof shared.removeMemory === 'function') shared.removeMemory(key);
  }
});

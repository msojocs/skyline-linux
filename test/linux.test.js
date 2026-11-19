'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');

const { linuxSharedMemoryDefault, firstExisting } = require('./lib/env');
const { uniqueKey } = require('./lib/util');
const { FrameReader, estimateSharedMemoryBytes, FRAME_HEADER_BYTES } = require('./lib/frame');

const sharedPath = firstExisting([
  process.env.SKYLINE_LINUX_SHARED_MEMORY,
  linuxSharedMemoryDefault(),
]);
const shared = sharedPath ? require(sharedPath) : null;
const skip = shared ? false : `Linux sharedMemory module not found (${sharedPath || 'not configured'})`;

function fileFor(key) {
  return `/dev/shm/skyline_${key}.dat`;
}

function cleanup(key) {
  try { shared.removeMemory(key); } catch {}
  try { fs.unlinkSync(fileFor(key)); } catch {}
}

function writeFrame(key, { frameId, width = 4, height = 4, dpr = 1 }) {
  const pixelBytes = Math.ceil(width * dpr) * Math.ceil(height * dpr) * 4;
  const total = FRAME_HEADER_BYTES + pixelBytes;
  const handle = shared.setMemory(key, total);
  const data = Buffer.from(shared.getMemory(key));
  data.writeUInt32LE(frameId, 0);
  data.writeUInt32LE(pixelBytes, 4);
  for (let index = 0; index < pixelBytes; index++) data[8 + index] = (index * 3 + 1) & 0xff;
  return { pixelBytes, total, handle };
}

test('Linux sharedMemory module is available', { skip }, () => {
  assert.equal(typeof shared.setMemory, 'function');
  assert.equal(typeof shared.getMemory, 'function');
  assert.equal(typeof shared.removeMemory, 'function');
  assert.equal(typeof shared.copyMemoryToBuffer, 'function');
  assert.equal(typeof shared.deleteCopyCache, 'function');
});

test('setMemory writes the 16-byte outer header and zero-initializes the data region', { skip }, () => {
  const key = uniqueKey('linux_header');
  try {
    const size = 256;
    const handle = shared.setMemory(key, size);
    assert.ok(handle instanceof ArrayBuffer, 'Linux setMemory returns an external ArrayBuffer');
    const bytes = fs.readFileSync(fileFor(key));
    assert.equal(bytes.length, 16 + size);
    assert.equal(Number(bytes.readBigUInt64LE(0)), size);
    assert.equal(bytes.readInt32LE(8), 1);
    assert.deepEqual(bytes.subarray(12, 16), Buffer.alloc(4));
    assert.ok(bytes.subarray(16).every(byte => byte === 0), 'data region must be zeroed');
  } finally {
    cleanup(key);
  }
});

test('setMemory validates keys and lengths', { skip }, () => {
  assert.throws(() => shared.setMemory('../escape', 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('', 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('a b', 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('x'.repeat(201), 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('ok_key-1.2', 0), RangeError);
  assert.throws(() => shared.setMemory('ok_key-1.2', -1), RangeError);
  assert.throws(() => shared.setMemory('ok_key-1.2', 0.5), RangeError);
  assert.throws(() => shared.setMemory('ok_key-1.2', NaN), RangeError);
  assert.throws(() => shared.setMemory('ok_key-1.2', Infinity), RangeError);
  assert.throws(() => shared.setMemory('ok_key-1.2', 2 ** 40), /超出支持范围|invalid/);

  const key = uniqueKey('linux_valid');
  try {
    const handle = shared.setMemory(key, 1);
    assert.ok(handle instanceof ArrayBuffer);
    assert.equal(handle.byteLength, 1);
  } finally {
    cleanup(key);
  }
});

test('reusing a key replaces the generation while keeping the header consistent', { skip }, () => {
  const key = uniqueKey('linux_reuse');
  try {
    shared.setMemory(key, 32);
    shared.setMemory(key, 64);
    const bytes = fs.readFileSync(fileFor(key));
    assert.equal(bytes.length, 16 + 64);
    assert.equal(Number(bytes.readBigUInt64LE(0)), 64);
    assert.equal(shared.getMemory(key).byteLength, 64);
  } finally {
    cleanup(key);
  }
});

test('FrameReader parses the 8-byte frame header and suppresses unchanged frames', { skip }, () => {
  const key = uniqueKey('linux_frame');
  try {
    const { pixelBytes } = writeFrame(key, { frameId: 7, width: 4, height: 4, dpr: 1 });
    const reader = new FrameReader(shared, key, { width: 4, height: 4, dpr: 1 });
    const first = reader.read();
    assert.equal(first.frameId, 7);
    assert.equal(first.frameSize, pixelBytes);
    assert.equal(first.changed, true);
    assert.equal(first.pixels.length, pixelBytes);
    for (let index = 0; index < pixelBytes; index++) {
      assert.equal(first.pixels[index], (index * 3 + 1) & 0xff);
    }
    const second = reader.read();
    assert.equal(second.changed, false, 'same frameId/frameSize must not trigger an upload');

    const data = Buffer.from(shared.getMemory(key));
    data.writeUInt32LE(8, 0);
    const third = reader.read();
    assert.equal(third.frameId, 8);
    assert.equal(third.changed, true);
  } finally {
    cleanup(key);
  }
});

test('FrameReader retries with requiredSize when the staging buffer is too small', { skip }, () => {
  const key = uniqueKey('linux_retry');
  try {
    const { total } = writeFrame(key, { frameId: 9, width: 4, height: 4, dpr: 1 });
    const reader = new FrameReader(shared, key, { width: 4, height: 4, dpr: 1, capacity: 4 });
    const frame = reader.read();
    assert.equal(frame.frameId, 9);
    assert.equal(reader.target.length, total);
  } finally {
    cleanup(key);
  }
});

test('FrameReader returns null for a missing shared memory key', { skip }, () => {
  const reader = new FrameReader(shared, uniqueKey('linux_missing'), { width: 4, height: 4, dpr: 1 });
  assert.equal(reader.read(), null);
});

test('estimateSharedMemoryBytes matches the DevTools formula', () => {
  assert.equal(estimateSharedMemoryBytes(100, 100, 2), 8 + 200 * 200 * 4);
  assert.equal(estimateSharedMemoryBytes(390, 844, 2), 8 + 780 * 1688 * 4);
  // ceil() is applied per axis.
  assert.equal(estimateSharedMemoryBytes(10.5, 10.5, 1.5), 8 + 16 * 16 * 4);
});

test('shared memory is visible across processes and the copy API observes writes', { skip }, async t => {
  const key = uniqueKey('linux_cross');
  const childFile = path.resolve(__dirname, 'fixtures', 'shm-child.js');
  const child = spawn(process.execPath, [childFile, sharedPath, key, '128'], { stdio: ['ignore', 'pipe', 'pipe'] });
  t.after(() => { try { child.kill('SIGKILL'); } catch {} cleanup(key); });
  let output = '';
  child.stdout.on('data', chunk => { output += chunk; });
  const readyDeadline = Date.now() + 10000;
  while (!output.includes('ready') && Date.now() < readyDeadline) {
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  assert.match(output, /ready/, `child did not become ready: ${output}`);

  const target = Buffer.alloc(128);
  assert.equal(shared.copyMemoryToBuffer(key, target), 128);
  assert.equal(target[0], 0x42);
  assert.equal(target[1], 0x43);
  assert.equal(target[127], 0x7f);

  // A second copy must observe a later write from the producer.
  const childViewUpdate = target;
  assert.equal(shared.copyMemoryToBuffer(key, childViewUpdate, 8), 8);
  assert.deepEqual([...childViewUpdate.subarray(0, 2)], [0x42, 0x43]);

  child.kill('SIGKILL');
  await once(child, 'exit').catch(() => {});
});

test('missing keys and removeMemory match the official Windows semantics', { skip }, () => {
  const missing = uniqueKey('linux_missing_semantics');
  assert.equal(shared.getMemory(missing), null);
  assert.equal(shared.removeMemory(missing), undefined);

  const key = uniqueKey('linux_remove_semantics');
  try {
    shared.setMemory(key, 16);
    assert.equal(shared.removeMemory(key), undefined);
  } finally {
    cleanup(key);
  }
});

test('legacy getMemory views stay valid after deleteCopyCache', { skip }, () => {
  const key = uniqueKey('linux_legacy');
  try {
    shared.setMemory(key, 32);
    const view = new Uint8Array(shared.getMemory(key));
    view[0] = 0x5a;
    shared.deleteCopyCache(key);
    const target = Buffer.alloc(32);
    assert.equal(shared.copyMemoryToBuffer(key, target), 32);
    assert.equal(target[0], 0x5a);
  } finally {
    cleanup(key);
  }
});

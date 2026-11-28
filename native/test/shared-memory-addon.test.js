'use strict';

// Regression tests for the in-repo sharedMemory addon (native/shared-memory).
//
// The addon is built by `npm run native:build` into native/build/sharedMemory.node.
// These cases mirror the upstream skyline-shared-memory test suite (api.js and
// copy-memory.js) and run as part of `npm test` when the addon is present.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const repoRoot = path.resolve(__dirname, '..', '..');

function findAddon() {
  const candidates = [
    process.env.SKYLINE_SHARED_MEMORY_MODULE,
    process.env.SKYLINE_LINUX_SHARED_MEMORY,
    path.join(repoRoot, 'native', 'build', 'sharedMemory.node'),
    path.join(repoRoot, 'native', 'build-release', 'sharedMemory.node'),
  ].filter(Boolean);
  return candidates.find((candidate) => fs.existsSync(candidate)) || null;
}

const modulePath = findAddon();
const skip = modulePath
  ? false
  : 'native/build/sharedMemory.node not found (run: npm run native:build)';
const shared = modulePath ? require(modulePath) : null;

let sequence = 0;
function uniqueKey(label) {
  sequence += 1;
  return `skyline_${label}_${process.pid}_${Date.now().toString(36)}_${sequence}`;
}

function fileFor(key) {
  return `/dev/shm/skyline_${key}.dat`;
}

function cleanup(key) {
  try { shared.removeMemory(key); } catch { /* already gone */ }
  try { fs.unlinkSync(fileFor(key)); } catch { /* not a file-backed key */ }
}

test('sharedMemory: addon exports the module surface', { skip }, () => {
  for (const name of [
    'setMemory',
    'getMemory',
    'removeMemory',
    'copyMemoryToBuffer',
    'deleteCopyCache',
    'version',
  ]) {
    assert.equal(typeof shared[name], 'function', `${name} should be a function`);
  }
  assert.equal(shared.version(), '1352');
});

test('sharedMemory: setMemory/getMemory exchange bytes through /dev/shm', { skip }, () => {
  const key = uniqueKey('roundtrip');
  try {
    const view = shared.setMemory(key, 64);
    assert.ok(view instanceof ArrayBuffer, 'Node returns the legacy external ArrayBuffer');
    assert.equal(view.byteLength, 64);

    const bytes = new Uint8Array(view);
    bytes[0] = 0x5a;
    bytes[63] = 0xa5;
    const observed = new Uint8Array(shared.getMemory(key));
    assert.equal(observed[0], 0x5a);
    assert.equal(observed[63], 0xa5);

    const file = fs.readFileSync(fileFor(key));
    assert.equal(file.length, 16 + 64);
    assert.equal(Number(file.readBigUInt64LE(0)), 64);
    assert.equal(file.readInt32LE(8), 1);
    assert.equal(file[16], 0x5a);
    assert.equal(file[79], 0xa5);
  } finally {
    cleanup(key);
  }
});

test('sharedMemory: setMemory validates keys, lengths, and missing keys', { skip }, () => {
  assert.throws(() => shared.setMemory('../escape', 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('', 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('a b', 16), /invalid shared memory key/);
  assert.throws(() => shared.setMemory('x'.repeat(201), 16), /invalid shared memory key/);

  for (const length of [0, -1, 0.5, NaN, Infinity]) {
    assert.throws(() => shared.setMemory('valid_key', length), RangeError);
  }
  assert.throws(() => shared.setMemory('valid_key', 2 ** 40), /超出支持范围|invalid/);

  assert.equal(shared.getMemory(uniqueKey('missing')), null);
  assert.equal(shared.removeMemory(uniqueKey('missing')), undefined);
});

test('sharedMemory: Electron handle mode returns a 16-byte descriptor', { skip }, () => {
  // The DevTools appservice deletes `process`, so detection falls back to the
  // user agent; the launch environment can force handle mode explicitly.  The
  // addon reads its environment once, so the mode is set on a fresh process.
  const key = uniqueKey('handle');
  const script = `
    const shared = require(process.env.HANDLE_MODULE);
    const handle = shared.setMemory(process.env.HANDLE_KEY, 32);
    const view = new DataView(handle.buffer, handle.byteOffset, handle.byteLength);
    console.log(JSON.stringify({
      typed: handle instanceof Uint8Array,
      length: handle.byteLength,
      pointer: String(view.getBigUint64(0, true)),
      size: String(view.getBigUint64(8, true)),
    }));
    shared.removeMemory(process.env.HANDLE_KEY);
  `;
  try {
    const result = spawnSync(process.execPath, ['-e', script], {
      encoding: 'utf8',
      timeout: 20000,
      env: {
        ...process.env,
        SKYLINE_SHARED_MEMORY_HANDLE: '1',
        HANDLE_MODULE: modulePath,
        HANDLE_KEY: key,
      },
    });
    assert.equal(result.status, 0, result.stderr);
    const handle = JSON.parse(result.stdout.trim().split('\n').pop());
    assert.ok(handle.typed, 'handle mode returns a typed array');
    assert.equal(handle.length, 16);
    assert.notEqual(handle.pointer, '0', 'pointer must be non-zero');
    assert.equal(handle.size, '32', 'size follows the pointer');
  } finally {
    cleanup(key);
  }
});

test('sharedMemory: copyMemoryToBuffer honors offsets, lengths, and cache resets', { skip }, () => {
  const key = uniqueKey('copy');
  const filename = fileFor(key);
  const payload = Buffer.from(Array.from({ length: 64 }, (_, i) => (i * 13 + 7) & 0xff));
  let fixtureExists = false;

  function writeFixture(bytes) {
    const file = Buffer.alloc(16 + bytes.length);
    file.writeBigUInt64LE(BigInt(bytes.length), 0);
    file.writeInt32LE(1, 8);
    bytes.copy(file, 16);
    fs.writeFileSync(filename, file, { flag: 'wx' });
    fixtureExists = true;
  }

  try {
    writeFixture(payload);

    // Buffer offset and trailing bytes stay untouched.
    const backing = Buffer.alloc(payload.length + 16, 0xa5);
    const target = backing.subarray(8, 8 + payload.length);
    assert.equal(shared.copyMemoryToBuffer(key, target), payload.length);
    assert.deepEqual(target, payload);
    assert.deepEqual(backing.subarray(0, 8), Buffer.alloc(8, 0xa5));
    assert.deepEqual(backing.subarray(-8), Buffer.alloc(8, 0xa5));

    // TypedArray views with byteOffset are supported.
    const arrayBuffer = new ArrayBuffer(payload.length + 16);
    new Uint8Array(arrayBuffer).fill(0x5c);
    const typed = new Uint32Array(arrayBuffer, 8, payload.length / 4);
    assert.equal(shared.copyMemoryToBuffer(key, typed), payload.length);
    assert.deepEqual(Buffer.from(arrayBuffer, 8, payload.length), payload);

    // A plain ArrayBuffer is also accepted.
    const plain = new ArrayBuffer(payload.length);
    assert.equal(shared.copyMemoryToBuffer(key, plain), payload.length);
    assert.deepEqual(Buffer.from(plain), payload);

    // Undersized targets report requiredSize without modifying the target.
    const small = Buffer.alloc(8, 0xcd);
    assert.throws(() => shared.copyMemoryToBuffer(key, small), (error) => {
      assert(error instanceof RangeError);
      assert.equal(error.requiredSize, payload.length);
      return true;
    });
    assert.deepEqual(small, Buffer.alloc(8, 0xcd));
    assert.equal(shared.copyMemoryToBuffer(key, small, 5), 5);
    assert.deepEqual(small.subarray(0, 5), payload.subarray(0, 5));
    assert.deepEqual(small.subarray(5), Buffer.alloc(3, 0xcd));
    assert.equal(shared.copyMemoryToBuffer(key, small, 0), 0);

    // Invalid lengths and arguments.
    for (const length of [-1, 0.5, NaN, Infinity, payload.length + 1]) {
      assert.throws(() => shared.copyMemoryToBuffer(key, target, length), RangeError);
    }
    assert.throws(() => shared.copyMemoryToBuffer(key, target, '8'), TypeError);
    assert.throws(() => shared.copyMemoryToBuffer(key, {}), TypeError);
    assert.throws(() => shared.copyMemoryToBuffer(key, Buffer.alloc(0)), RangeError);
    assert.throws(() => shared.copyMemoryToBuffer('../invalid', target), TypeError);
    assert.throws(() => shared.deleteCopyCache('../invalid'), TypeError);
    assert.throws(() => shared.deleteCopyCache(), TypeError);
    assert.equal(shared.copyMemoryToBuffer(uniqueKey('missing'), target), null);

    // Repeated reads observe writes through the same mapping.
    const changed = Buffer.alloc(payload.length, 0x3c);
    const fd = fs.openSync(filename, 'r+');
    try {
      fs.writeSync(fd, changed, 0, changed.length, fs.statSync(filename).size - changed.length);
    } finally {
      fs.closeSync(fd);
    }
    assert.equal(shared.copyMemoryToBuffer(key, target), payload.length);
    assert.deepEqual(target, changed);

    // Clearing the cache releases the mapping but not the producer's file.
    shared.deleteCopyCache(key);
    shared.deleteCopyCache(key);
    assert.deepEqual(target, changed);
    assert.ok(fs.existsSync(filename));
    assert.equal(shared.copyMemoryToBuffer(key, target), changed.length);

    // A replaced file is only picked up after deleteCopyCache.
    fs.unlinkSync(filename);
    fixtureExists = false;
    const replacement = Buffer.alloc(payload.length * 2, 0x7e);
    writeFixture(replacement);
    assert.equal(shared.copyMemoryToBuffer(key, target), changed.length);
    assert.deepEqual(target, changed);
    shared.deleteCopyCache(key);
    assert.throws(
      () => shared.copyMemoryToBuffer(key, target),
      error => error.requiredSize === replacement.length,
    );
    const larger = Buffer.alloc(replacement.length);
    assert.equal(shared.copyMemoryToBuffer(key, larger), replacement.length);
    assert.deepEqual(larger, replacement);

    // Legacy getMemory views survive clearing the independent copy cache.
    const legacy = new Uint8Array(shared.getMemory(key));
    shared.deleteCopyCache(key);
    legacy[0] = 0x42;
    assert.equal(shared.copyMemoryToBuffer(key, larger), replacement.length);
    assert.equal(larger[0], 0x42);
  } finally {
    try { shared.deleteCopyCache(key); } catch { /* nothing cached */ }
    if (fixtureExists) fs.unlinkSync(filename);
  }
});

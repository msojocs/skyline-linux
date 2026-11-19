'use strict';

// Cross-platform shared-memory and frame-protocol tests.
//
// The official Windows module (under Wine) stores shared memory as a Boost
// Interprocess object; Wine materialises it as
//   <WINEPREFIX>/drive_c/ProgramData/boost_interprocess/<bucket>/<key>
// containing exactly the data region (no outer file header).
//
// The Linux module stores a 16-byte {u64 size, i32 version, padding} header
// followed by the same data region in /dev/shm/skyline_<key>.dat.  Both expose
// the data region (frame header + pixels) through the public API, so the data
// layouts must be byte-identical.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');

const { oraclePaths, wineBinary, toWindowsPath, linuxSharedMemoryDefault } = require('./lib/env');
const { uniqueKey } = require('./lib/util');
const { parseFrame, FRAME_HEADER_BYTES } = require('./lib/frame');

const oracle = oraclePaths();
const sharedPath = linuxSharedMemoryDefault();
const shared = sharedPath ? require(sharedPath) : null;
const fixture = path.resolve(__dirname, 'fixtures', 'win-shm.js');
const winePrefix = process.env.WINEPREFIX || path.join(os.homedir(), '.wine');
const boostRoot = path.join(winePrefix, 'drive_c', 'ProgramData', 'boost_interprocess');

const skip = !oracle || !shared
  ? `requires Wine + official Windows resources (oracle=${!!oracle}) and a Linux sharedMemory module (${sharedPath || 'missing'})`
  : false;

function payloadFor(size, frame) {
  const buffer = Buffer.alloc(size);
  if (frame) {
    const pixels = size - FRAME_HEADER_BYTES;
    buffer.writeUInt32LE(frame.frameId, 0);
    buffer.writeUInt32LE(pixels, 4);
    for (let index = 0; index < pixels; index++) {
      buffer[FRAME_HEADER_BYTES + index] = (index * frame.seed + 11) & 0xff;
    }
  } else {
    for (let index = 0; index < size; index++) buffer[index] = (index * 7 + 3) & 0xff;
  }
  return buffer;
}

function signalPath(tag) {
  return path.join(os.tmpdir(), `skyline-cross-${process.pid}-${tag}.json`);
}

async function waitForSignal(file, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (fs.existsSync(file)) {
      try {
        const text = fs.readFileSync(file, 'utf8');
        if (text.trim()) return JSON.parse(text);
      } catch {
        // still being written
      }
    }
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error(`timed out waiting for ${file}`);
}

function startWineFixture({ mode, key, size, signal, frame, extraEnv = {} }) {
  const log = `${signal}.log`;
  const logFd = fs.openSync(log, 'w');
  const args = [
    toWindowsPath(fixture),
  ];
  const child = spawn(wineBinary(), [oracle.electron, ...args], {
    env: {
      ...process.env,
      WINEDEBUG: '-all',
      ELECTRON_RUN_AS_NODE: '1',
      SHM_MODULE: toWindowsPath(oracle.shared),
      SHM_KEY: key,
      SHM_SIZE: String(size),
      SHM_SIGNAL: toWindowsPath(signal),
      SHM_MODE: mode,
      SHM_FRAME: frame ? '1' : '0',
      SHM_FRAME_ID: frame ? String(frame.frameId) : '0',
      SHM_SEED: frame ? String(frame.seed) : '1',
      ...extraEnv,
    },
    stdio: ['ignore', logFd, logFd],
  });
  child.on('close', () => {
    try { fs.closeSync(logFd); } catch {}
  });
  return { child, log };
}

async function stopChild(child) {
  if (!child || child.exitCode !== null) return;
  try { child.kill('SIGKILL'); } catch {}
  await new Promise(resolve => {
    if (child.exitCode !== null) return resolve();
    child.once('close', resolve);
    setTimeout(resolve, 2000);
  });
}

function findBoostFile(key) {
  if (!fs.existsSync(boostRoot)) return null;
  for (const bucket of fs.readdirSync(boostRoot)) {
    const candidate = path.join(boostRoot, bucket, key);
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

function boostBucketDir() {
  if (fs.existsSync(boostRoot)) {
    for (const bucket of fs.readdirSync(boostRoot)) {
      const full = path.join(boostRoot, bucket);
      try { if (fs.statSync(full).isDirectory()) return full; } catch {}
    }
  }
  const fallback = path.join(boostRoot, '01000000');
  fs.mkdirSync(fallback, { recursive: true });
  return fallback;
}

test('official Windows module stores the data region without an outer header', { skip, timeout: 30000 }, async t => {
  const key = uniqueKey('cross_win');
  const size = 8 + 16 * 16 * 4;
  const frame = { frameId: 4242, seed: 3 };
  const signal = signalPath('producer');
  try { fs.unlinkSync(signal); } catch {}
  const { child, log } = startWineFixture({ mode: 'produce', key, size, signal, frame });
  t.after(async () => {
    await stopChild(child);
    try { fs.unlinkSync(signal); } catch {}
  });
  const ready = await waitForSignal(signal);
  assert.equal(ready.error, undefined, `producer failed: ${ready.error} (${log})`);
  assert.ok(ready.exports.includes('setMemoryByAddress'));

  const file = findBoostFile(key);
  assert.ok(file, `boost interprocess file not found under ${boostRoot}`);
  const data = fs.readFileSync(file);
  const expected = payloadFor(size, frame);
  assert.equal(data.length, size, 'boost object contains exactly the data region');
  assert.deepEqual(data, expected, 'official producer data region matches the frame layout');

  const parsed = parseFrame(data);
  assert.equal(parsed.frameId, frame.frameId);
  assert.equal(parsed.frameSize, size - FRAME_HEADER_BYTES);
  assert.equal(parsed.pixels[0], 11);
  assert.equal(parsed.pixels[1], 14);
});

test('Linux shared memory is readable by the official Windows module', { skip, timeout: 30000 }, async t => {
  const key = uniqueKey('cross_linux');
  const size = 8 + 8 * 8 * 4;
  const frame = { frameId: 77, seed: 5 };
  const expected = payloadFor(size, frame);
  const shmFile = `/dev/shm/skyline_${key}.dat`;
  const signal = signalPath('reader');
  let boostFile = null;
  try { fs.unlinkSync(signal); } catch {}

  try {
    shared.setMemory(key, size);
    const data = Buffer.from(shared.getMemory(key));
    expected.copy(data);
    assert.equal(data.length, size);

    const raw = fs.readFileSync(shmFile);
    assert.equal(raw.length, 16 + size);
    assert.equal(Number(raw.readBigUInt64LE(0)), size);
    assert.equal(raw.readInt32LE(8), 1);
    assert.deepEqual(raw.subarray(16), expected, 'the file body is the data region');

    // Materialise the data region as a Boost Interprocess object and let the
    // official module open it by key.
    boostFile = path.join(boostBucketDir(), key);
    fs.writeFileSync(boostFile, expected);

    const { child, log } = startWineFixture({ mode: 'read', key, size, signal });
    t.after(() => stopChild(child));
    const result = await waitForSignal(signal);
    assert.equal(result.error, undefined, `reader failed: ${result.error} (${log})`);
    assert.equal(result.length, size);
    assert.equal(result.hash, crypto.createHash('sha256').update(expected).digest('hex'));
    assert.deepEqual(result.first, [...expected.subarray(0, 8)]);
    assert.deepEqual(result.last, [...expected.subarray(size - 4)]);
  } finally {
    try { shared.removeMemory(key); } catch {}
    try { fs.unlinkSync(shmFile); } catch {}
    if (boostFile) { try { fs.unlinkSync(boostFile); } catch {} }
    try { fs.unlinkSync(signal); } catch {}
  }
});

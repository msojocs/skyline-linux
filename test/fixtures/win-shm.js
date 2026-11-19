'use strict';

// Wine-side fixture for cross-platform shared-memory tests.
//   MODE=produce: create a region, write a payload, signal readiness, stay alive.
//   MODE=read:    open an existing region, hash the data region, signal, exit.
// All paths crossing the Wine boundary are provided as Windows paths.

const fs = require('node:fs');
const crypto = require('node:crypto');

const shared = require(process.env.SHM_MODULE);
const key = process.env.SHM_KEY;
const signal = process.env.SHM_SIGNAL;
const size = Number(process.env.SHM_SIZE || 64);
const mode = process.env.SHM_MODE || 'produce';

function pattern(index) {
  return (index * 7 + 3) & 0xff;
}

function payloadBytes() {
  const buffer = Buffer.alloc(size);
  if (process.env.SHM_FRAME === '1') {
    const pixels = size - 8;
    buffer.writeUInt32LE(Number(process.env.SHM_FRAME_ID || 1), 0);
    buffer.writeUInt32LE(pixels, 4);
    const seed = Number(process.env.SHM_SEED || 1);
    for (let index = 0; index < pixels; index++) buffer[8 + index] = (index * seed + 11) & 0xff;
  } else {
    for (let index = 0; index < size; index++) buffer[index] = pattern(index);
  }
  return buffer;
}

try {
  if (mode === 'produce') {
    const handle = shared.setMemory(key, size);
    shared.setMemoryByAddress(handle, payloadBytes());
    fs.writeFileSync(signal, JSON.stringify({
      mode,
      exports: Object.keys(shared).sort(),
      handleByteLength: handle.byteLength || handle.length,
      size,
    }));
    setInterval(() => {}, 1000);
  } else {
    const value = shared.getMemory(key);
    const buffer = value instanceof ArrayBuffer
      ? Buffer.from(value)
      : Buffer.from(value.buffer, value.byteOffset || 0, value.byteLength);
    fs.writeFileSync(signal, JSON.stringify({
      mode,
      length: buffer.length,
      hash: crypto.createHash('sha256').update(buffer).digest('hex'),
      first: [...buffer.subarray(0, 8)],
      last: [...buffer.subarray(buffer.length - 4)],
    }));
    process.exit(0);
  }
} catch (error) {
  fs.writeFileSync(signal, JSON.stringify({ mode, error: String(error && error.message || error) }));
  process.exit(1);
}

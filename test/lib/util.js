'use strict';

const crypto = require('node:crypto');

// A unique, valid shared-memory key for one case run.  Keys must stay within
// the modules' accepted alphabet: [A-Za-z0-9._-], at most 200 characters.
function uniqueKey(prefix = 'case') {
  return `${prefix}_${process.pid}_${crypto.randomBytes(6).toString('hex')}`;
}

// Turn an arbitrary JS value into a stable, JSON-friendly observation.  Native
// return values are often opaque (ArrayBuffer, typed arrays, function objects);
// we record their shape instead of their contents unless a case opts in.
function stabilize(value, depth = 0) {
  if (depth > 6) return '<depth-limit>';
  if (value === undefined) return '<undefined>';
  if (value === null) return null;
  const type = typeof value;
  if (type === 'string' || type === 'boolean') return value;
  if (type === 'number') {
    if (Number.isNaN(value)) return '<NaN>';
    if (!Number.isFinite(value)) return value > 0 ? '<Infinity>' : '<-Infinity>';
    return value;
  }
  if (type === 'bigint') return `<bigint:${value}>`;
  if (type === 'symbol') return '<symbol>';
  if (type === 'function') {
    return { kind: 'function', name: value.name, length: value.length };
  }
  if (value instanceof Error) {
    return { kind: 'error', name: value.name, message: String(value.message) };
  }
  if (ArrayBuffer.isView(value)) {
    const view = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    return {
      kind: value.constructor && value.constructor.name || 'TypedArray',
      byteLength: view.byteLength,
      // Small byte views (e.g. the 16-byte Windows memory handle) are useful
      // verbatim; keep them so the contract can pin the encoding.
      bytes: view.byteLength <= 32 ? Array.from(view) : undefined,
    };
  }
  if (value instanceof ArrayBuffer) {
    return { kind: 'ArrayBuffer', byteLength: value.byteLength };
  }
  if (Array.isArray(value)) return value.map(item => stabilize(item, depth + 1));
  if (type === 'object') {
    const result = {};
    for (const key of Object.keys(value).sort()) result[key] = stabilize(value[key], depth + 1);
    return result;
  }
  return `<${type}>`;
}

function capture(fn) {
  try {
    const value = fn();
    if (value && typeof value.then === 'function') {
      return { ok: false, error: { name: 'Error', message: 'async case returned a Promise; driver must await run()' } };
    }
    return { ok: true, value: stabilize(value) };
  } catch (error) {
    return {
      ok: false,
      error: {
        name: error && error.name || typeof error,
        message: String(error && error.message !== undefined ? error.message : error),
      },
    };
  }
}

async function captureAsync(fn) {
  try {
    return { ok: true, value: stabilize(await fn()) };
  } catch (error) {
    return {
      ok: false,
      error: {
        name: error && error.name || typeof error,
        message: String(error && error.message !== undefined ? error.message : error),
      },
    };
  }
}

function descriptorShape(value) {
  if (value instanceof ArrayBuffer) {
    return { kind: 'ArrayBuffer', byteLength: value.byteLength };
  }
  if (ArrayBuffer.isView(value)) {
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const result = {
      kind: value.constructor && value.constructor.name || 'TypedArray',
      byteLength: bytes.byteLength,
    };
    if (bytes.byteLength === 16) {
      // The first 8 bytes are a process-local pointer; record only whether it
      // is non-zero so Windows/Linux observations stay comparable.
      result.pointerNonZero = Buffer.from(bytes.buffer, bytes.byteOffset, 8).readBigUInt64LE(0) !== 0n;
      result.size = Number(Buffer.from(bytes.buffer, bytes.byteOffset + 8, 8).readBigUInt64LE(0));
    }
    return result;
  }
  return { kind: typeof value };
}

module.exports = { uniqueKey, stabilize, capture, captureAsync, descriptorShape };

'use strict';

// L5 frame comparison helpers.
//
// The Linux-side baseline stores a deterministic SHA-256 for a fixture frame.
// When an official Windows devtools/appservice guest becomes available, the
// same fixture can be recorded into `spec/l5-baseline.win32.json` and compared
// with a per-channel tolerance (fonts/anti-aliasing differ between Skia builds).

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { decodePng, encodePng } = require('./png');

function sha256(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

// Compares two RGBA frames.  `tolerance` is the maximum allowed absolute
// difference per channel (0 = exact).  `maxMismatchRatio` bounds the fraction
// of channels that may exceed the tolerance.  `ignoreRects` is an optional
// list of `[x0, y0, x1, y1]` device-pixel rectangles that are excluded from
// the comparison (used for the Linux debug-build banner).
function compareFrames(expected, actual, options = {}) {
  const tolerance = Number.isInteger(options.tolerance) ? options.tolerance : 0;
  const maxMismatchRatio =
    typeof options.maxMismatchRatio === 'number' ? options.maxMismatchRatio : 0;
  const expectedBytes = Buffer.from(expected);
  const actualBytes = Buffer.from(actual);
  const expectedHash = sha256(expectedBytes);
  const actualHash = sha256(actualBytes);
  if (expectedBytes.length !== actualBytes.length) {
    return {
      equal: false,
      expectedHash,
      actualHash,
      lengthMismatch: true,
      maxDelta: 255,
      mismatchedChannels: expectedBytes.length,
      mismatchRatio: 1,
      passed: false,
    };
  }
  const ignoreRects = Array.isArray(options.ignoreRects) ? options.ignoreRects : [];
  const width = Number.isInteger(options.width) ? options.width : 0;
  const ignored = (x, y) => ignoreRects.some(
    ([x0, y0, x1, y1]) => x >= x0 && x < x1 && y >= y0 && y < y1,
  );
  let mismatched = 0;
  let maxDelta = 0;
  let comparedChannels = 0;
  for (let i = 0; i < expectedBytes.length; i += 4) {
    if (width > 0 && ignoreRects.length > 0) {
      const pixel = i / 4;
      if (ignored(pixel % width, Math.floor(pixel / width))) continue;
    }
    for (let channel = 0; channel < 4; channel++) {
      comparedChannels++;
      const delta = Math.abs(expectedBytes[i + channel] - actualBytes[i + channel]);
      if (delta > maxDelta) maxDelta = delta;
      if (delta > tolerance) mismatched++;
    }
  }
  const mismatchRatio =
    comparedChannels === 0 ? 0 : mismatched / comparedChannels;
  return {
    equal: mismatched === 0,
    expectedHash,
    actualHash,
    lengthMismatch: false,
    maxDelta,
    mismatchedChannels: mismatched,
    comparedChannels,
    mismatchRatio,
    passed: mismatched === 0 || mismatchRatio <= maxMismatchRatio,
  };
}

function samplePixel(pixels, width, x, y) {
  const offset = (y * width + x) * 4;
  return Array.from(pixels.subarray(offset, offset + 4));
}

// RGBA of one spec/l5-baseline.win32.json entry: the PNG named by `frame`
// (relative to the baseline file) or a legacy inline `rgbaZlib` blob.
function windowsFrame(entry, baselineFile) {
  if (entry.frame) {
    return decodePng(fs.readFileSync(path.resolve(path.dirname(baselineFile), entry.frame))).rgba;
  }
  if (typeof entry.rgbaZlib === 'string') {
    return zlib.inflateSync(Buffer.from(entry.rgbaZlib, 'base64'));
  }
  return null;
}

// Compares a Linux frame with the Windows frame of one baseline entry, using
// the entry's tolerance and mismatch budget (falling back to the baseline's)
// and the baseline's ignore rectangles.
function compareWithWindows(baseline, entry, windows, linux) {
  const dpr = baseline.dpr || 2;
  const width = baseline.frame_width || Math.ceil(baseline.width * dpr);
  const tolerance = Number.isInteger(entry.tolerance) ? entry.tolerance
    : Number.isInteger(baseline.tolerance) ? baseline.tolerance : 12;
  const maxMismatchRatio = typeof entry.maxMismatchRatio === 'number'
    ? entry.maxMismatchRatio : baseline.maxMismatchRatio ?? 0.01;
  const result = compareFrames(windows, linux, {
    width,
    tolerance,
    maxMismatchRatio,
    ignoreRects: baseline.ignoreRects || [],
  });
  return { ...result, width, tolerance, maxMismatchRatio };
}

// Writes <dir>/<name>.{expected,actual,diff}.png.  The diff shows the expected
// frame dimmed, with pixels whose channels differ by more than `tolerance` in
// red.  Returns the diff path.
function writeDiffImages(dir, name, expected, actual, width, tolerance = 0) {
  const height = expected.length / 4 / width;
  const diff = Buffer.alloc(expected.length);
  for (let i = 0; i < expected.length; i += 4) {
    let delta = 0;
    for (let channel = 0; channel < 4; channel++) {
      delta = Math.max(delta, Math.abs(expected[i + channel] - actual[i + channel]));
    }
    if (delta > tolerance) {
      diff.set([255, 0, 0, 255], i);
    } else {
      const gray = Math.round((expected[i] + expected[i + 1] + expected[i + 2]) / 3 * 0.3 + 178);
      diff.set([gray, gray, gray, 255], i);
    }
  }
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, `${name}.expected.png`), encodePng(expected, width, height));
  fs.writeFileSync(path.join(dir, `${name}.actual.png`), encodePng(actual, width, height));
  const file = path.join(dir, `${name}.diff.png`);
  fs.writeFileSync(file, encodePng(diff, width, height));
  return file;
}

// JSON with readable diffs: objects one key per line, arrays of primitives
// (or of primitive arrays, such as pixel samples) on a single line.
function stringifyBaseline(value, indent = '') {
  const primitive = item => item === null || typeof item !== 'object';
  if (Array.isArray(value)) {
    if (value.every(item => primitive(item) || (Array.isArray(item) && item.every(primitive)))) {
      return JSON.stringify(value);
    }
    const inner = `${indent}  `;
    return `[\n${value.map(item => inner + stringifyBaseline(item, inner)).join(',\n')}\n${indent}]`;
  }
  if (value && typeof value === 'object') {
    const entries = Object.entries(value).filter(([, item]) => item !== undefined);
    if (entries.length === 0) return '{}';
    const inner = `${indent}  `;
    return `{\n${entries.map(([key, item]) =>
      `${inner}${JSON.stringify(key)}: ${stringifyBaseline(item, inner)}`).join(',\n')}\n${indent}}`;
  }
  return JSON.stringify(value);
}

module.exports = {
  compareFrames,
  compareWithWindows,
  samplePixel,
  sha256,
  stringifyBaseline,
  windowsFrame,
  writeDiffImages,
};

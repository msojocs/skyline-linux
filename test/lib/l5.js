'use strict';

// L5 frame comparison helpers.
//
// The Linux-side baseline stores a deterministic SHA-256 for a fixture frame.
// When an official Windows devtools/appservice guest becomes available, the
// same fixture can be recorded into `spec/l5-baseline.win32.json` and compared
// with a per-channel tolerance (fonts/anti-aliasing differ between Skia builds).

const crypto = require('node:crypto');

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

module.exports = { sha256, compareFrames, samplePixel };

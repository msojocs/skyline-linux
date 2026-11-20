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
// of channels that may exceed the tolerance.
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
  let mismatched = 0;
  let maxDelta = 0;
  for (let i = 0; i < expectedBytes.length; i++) {
    const delta = Math.abs(expectedBytes[i] - actualBytes[i]);
    if (delta > maxDelta) maxDelta = delta;
    if (delta > tolerance) mismatched++;
  }
  const mismatchRatio =
    expectedBytes.length === 0 ? 0 : mismatched / expectedBytes.length;
  return {
    equal: mismatched === 0,
    expectedHash,
    actualHash,
    lengthMismatch: false,
    maxDelta,
    mismatchedChannels: mismatched,
    mismatchRatio,
    passed: mismatched === 0 || mismatchRatio <= maxMismatchRatio,
  };
}

function samplePixel(pixels, width, x, y) {
  const offset = (y * width + x) * 4;
  return Array.from(pixels.subarray(offset, offset + 4));
}

module.exports = { sha256, compareFrames, samplePixel };

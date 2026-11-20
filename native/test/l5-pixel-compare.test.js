'use strict';

// L5 pixel comparison harness.
//
// Linux-side frames are deterministic for the committed fixtures and are
// checked exactly against `spec/l5-baseline.json`.
//
// The Windows half needs the official DevTools appservice guest
// (docs/feasibility.md section 3).  When `spec/l5-baseline.win32.json` (or
// `SKYLINE_L5_WINDOWS_BASELINE`) is supplied it is compared with a per-channel
// tolerance; otherwise the comparison is reported as skipped.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..');
const l5 = require(path.join(repoRoot, 'tools', 'l5-baseline.js'));
const { compareFrames, sha256 } = require(path.join(repoRoot, 'test', 'lib', 'l5.js'));

function skipReason() {
  const missing = l5.prerequisites();
  return missing.length ? `missing ${missing.join(', ')}` : false;
}

function windowsBaselinePath() {
  const env = process.env.SKYLINE_L5_WINDOWS_BASELINE;
  if (env) return env;
  const candidate = path.join(repoRoot, 'spec', 'l5-baseline.win32.json');
  return fs.existsSync(candidate) ? candidate : null;
}

test('L5: Linux frame baseline is deterministic', {
  skip: skipReason(),
  timeout: 120000,
}, () => {
  assert.ok(fs.existsSync(l5.baselinePath),
    `missing ${l5.baselinePath}; run node tools/l5-baseline.js --write`);
  const baseline = JSON.parse(fs.readFileSync(l5.baselinePath, 'utf8'));
  const entries = Object.entries(baseline.fixtures || {});
  assert.ok(entries.length > 0, 'baseline has no fixtures');
  for (const [name, expected] of entries) {
    const fixture = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
    if (!fs.existsSync(fixture)) continue;
    const pixels = l5.render(fixture);
    assert.equal(sha256(pixels), expected.sha256,
      `${name} frame hash changed; regenerate the baseline if the change is intended`);
  }
});

test('L5: Windows baseline comparison uses a channel tolerance', {
  skip: skipReason() || (windowsBaselinePath() === null
    ? 'official appservice guest baseline not available (docs/feasibility.md section 3)'
    : false),
  timeout: 120000,
}, () => {
  const baselinePath = windowsBaselinePath();
  const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));
  const tolerance = Number.isInteger(baseline.tolerance) ? baseline.tolerance : 12;
  const fixtures = baseline.fixtures || {};
  let compared = 0;
  for (const [name, expected] of Object.entries(fixtures)) {
    const fixture = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
    if (!fs.existsSync(fixture)) continue;
    const pixels = l5.render(fixture);
    if (typeof expected.sha256 === 'string') {
      // A supplied Windows hash is only informational: exact equality is not
      // expected across Skia builds.
    }
    const samples = expected.samples || [];
    for (const [x, y, rgba] of samples) {
      const offset = (y * l5.width + x) * 4;
      for (let channel = 0; channel < 4; channel++) {
        const delta = Math.abs(pixels[offset + channel] - rgba[channel]);
        assert.ok(delta <= tolerance,
          `${name} (${x},${y}) channel ${channel}: |${pixels[offset + channel]} - ` +
          `${rgba[channel]}| = ${delta} > ${tolerance}`);
      }
    }
    if (Array.isArray(expected.rgba)) {
      const result = compareFrames(Buffer.from(expected.rgba), pixels, {
        tolerance,
        maxMismatchRatio: baseline.maxMismatchRatio ?? 0.01,
      });
      assert.ok(result.passed,
        `${name} frame differs: maxDelta=${result.maxDelta}, ` +
        `mismatchRatio=${result.mismatchRatio}`);
    }
    compared++;
  }
  assert.ok(compared > 0, 'Windows baseline has no comparable fixtures');
});

'use strict';

// L5 pixel comparison harness.
//
// Linux-side frames are deterministic for the committed fixtures and are
// checked exactly against `spec/l5-baseline.json`.
//
// The Windows half uses frames captured by `tools/l5-windows.js` from the
// official Windows `skyline.node` under Wine (dpr=2).  `spec/l5-baseline.win32.json`
// stores the zlib-compressed RGBA frames; comparison uses a per-channel
// tolerance and excludes the Linux debug build's corner banner.
// See docs/feasibility.md section 3.1.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

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

function fixturePath(name) {
  const fixture = path.join(l5.fixturesDir, `${name}.ops.json`);
  assert.ok(fs.existsSync(fixture), `${name}: fixture missing (${fixture})`);
  return fixture;
}

// Engine-free: a fixture that is added without a baseline entry (or a stale
// baseline entry) must fail here instead of being skipped by the frame tests.
test('L5: fixture lists and baselines cover the same fixtures', () => {
  assert.ok(fs.existsSync(l5.baselinePath), `missing ${l5.baselinePath}; run npm run l5:baseline`);
  const baselines = {
    'spec/l5-baseline.json': JSON.parse(fs.readFileSync(l5.baselinePath, 'utf8')),
  };
  const winPath = windowsBaselinePath();
  if (winPath) baselines[path.relative(repoRoot, winPath)] = JSON.parse(fs.readFileSync(winPath, 'utf8'));
  const problems = l5.fixtureSetProblems(baselines);
  assert.deepEqual(problems, [],
    `L5 fixture sets disagree:\n  ${problems.join('\n  ')}\n` +
    'List new fixtures in FIXTURES or EXCLUDED_FIXTURES (tools/l5-baseline.js), then run ' +
    'npm run l5:baseline and npm run l5:windows -- --only <name> --write.');
});

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
    const pixels = l5.render(fixturePath(name));
    assert.equal(sha256(pixels), expected.sha256,
      `${name} frame hash changed; regenerate the baseline if the change is intended`);
  }
});

test('L5: Windows baseline comparison uses a channel tolerance', {
  skip: skipReason() || (windowsBaselinePath() === null
    ? 'missing spec/l5-baseline.win32.json (run npm run l5:windows)'
    : false),
  timeout: 240000,
}, () => {
  const baselinePath = windowsBaselinePath();
  const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));
  const dpr = baseline.dpr || 2;
  const frameWidth = baseline.frame_width || Math.ceil(baseline.width * dpr);
  const frameHeight = baseline.frame_height || Math.ceil(baseline.height * dpr);
  const frameSize = frameWidth * frameHeight * 4;
  const tolerance = Number.isInteger(baseline.tolerance) ? baseline.tolerance : 12;
  const ignoreRects = Array.isArray(baseline.ignoreRects) ? baseline.ignoreRects : [];
  const ignored = (x, y) => ignoreRects.some(
    ([x0, y0, x1, y1]) => x >= x0 && x < x1 && y >= y0 && y < y1,
  );
  const fixtures = baseline.fixtures || {};
  let compared = 0;
  let skipped = 0;
  for (const [name, expected] of Object.entries(fixtures)) {
    const fixture = fixturePath(name);
    if (expected.compare === false) {
      skipped++;
      continue;
    }
    const pixels = l5.render(fixture, {
      dpr,
      width: baseline.width,
      height: baseline.height,
    });
    assert.equal(pixels.length, frameSize,
      `${name}: Linux frame is ${pixels.length} bytes, expected ${frameSize}`);
    if (typeof expected.rgbaZlib === 'string') {
      const win = zlib.inflateSync(Buffer.from(expected.rgbaZlib, 'base64'));
      assert.equal(win.length, frameSize,
        `${name}: Windows baseline frame has the wrong size`);
      assert.equal(sha256(win), expected.sha256,
        `${name}: Windows baseline frame hash is corrupt`);
      const result = compareFrames(win, pixels, {
        width: frameWidth,
        tolerance: Number.isInteger(expected.tolerance) ? expected.tolerance : tolerance,
        maxMismatchRatio: typeof expected.maxMismatchRatio === 'number'
          ? expected.maxMismatchRatio
          : baseline.maxMismatchRatio ?? 0.01,
        ignoreRects,
      });
      assert.ok(result.passed,
        `${name} frame differs: maxDelta=${result.maxDelta}, ` +
        `mismatchRatio=${result.mismatchRatio.toFixed(6)}` +
        (expected.note ? ` (${expected.note})` : ''));
    }
    for (const [x, y, rgba] of (expected.samplesInformational ? [] : (expected.samples || []))) {
      if (ignored(x, y)) continue;
      const offset = (y * frameWidth + x) * 4;
      for (let channel = 0; channel < 4; channel++) {
        const delta = Math.abs(pixels[offset + channel] - rgba[channel]);
        assert.ok(delta <= tolerance,
          `${name} (${x},${y}) channel ${channel}: |${pixels[offset + channel]} - ` +
          `${rgba[channel]}| = ${delta} > ${tolerance}`);
      }
    }
    compared++;
  }
  assert.ok(compared > 0, 'Windows baseline has no comparable fixtures');
  if (skipped > 0) {
    // Known build-path divergence; the frame is still committed for inspection.
    console.log(`L5: skipped ${skipped} known-divergent fixtures`);
  }
});

'use strict';

// Captures L5 fixture frames with the official Windows Skyline engine under
// Wine, or compares a previously captured baseline with the Linux engine.
//
//   node tools/l5-windows.js --write      # capture + write spec/l5-baseline.win32.json
//   node tools/l5-windows.js              # compare Linux frames with the baseline
//   node tools/l5-windows.js --reuse --write
//
// The official engine only accepts device_pixel_ratio == 2, so frames are
// captured at 320x240 CSS pixels and stored as 640x480 RGBA.  The raw shared
// memory buffer is BGRA on Windows; this tool swaps red/blue before hashing.
//
// The Linux build used for comparison is a debug engine and paints the Flutter
// "DEBUG" banner in the top-right corner; that rectangle is excluded from the
// comparison via `ignoreRects`.

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const zlib = require('node:zlib');

const repoRoot = path.resolve(__dirname, '..');
const appDir = path.join(repoRoot, 'tools', 'l5-win-app');
const workDir = path.join(repoRoot, 'tmp', 'l5-win');
const shimDir = path.join(workDir, 'electron');
const frameDir = path.join(workDir, 'frames');
const baselinePath = path.join(repoRoot, 'spec', 'l5-baseline.win32.json');
const l5 = require(path.join(__dirname, 'l5-baseline.js'));
const { compareFrames } = require(path.join(repoRoot, 'test', 'lib', 'l5.js'));
const { windowsResources, windowsElectron, wineBinary } = require(
  path.join(repoRoot, 'test', 'lib', 'env.js'));

const LOGICAL_WIDTH = 320;
const LOGICAL_HEIGHT = 240;
const DPR = 2;
const FRAME_WIDTH = LOGICAL_WIDTH * DPR;
const FRAME_HEIGHT = LOGICAL_HEIGHT * DPR;
const FRAME_SIZE = FRAME_WIDTH * FRAME_HEIGHT * 4;
const TOLERANCE = 12;
// The Linux debug build's corner banner; the released Windows engine does not
// paint it.  Coordinates are device pixels.
const IGNORE_RECTS = [[500, 0, 640, 140]];

// Fixtures whose synthetic op stream cannot drive the official engine's native
// builder path (or whose painted result is still an open fidelity gap).
const KNOWN_DIVERGENCES = {
  'appservice-transform3d': {
    note: 'Linux 3D transform fidelity differs from the official engine',
    compare: true,
    headroom: 0.05,
    samplesInformational: true,
  },
  'appservice-overflow': {
    note: 'rounded clip edges differ by at most 109/255; mismatch ratio < 0.2%',
    compare: true,
    headroom: 0.01,
    samplesInformational: true,
  },
  'appservice-transform-origin-z': {
    note: 'official engine ignores the transform-origin z component (3D projection also differs)',
    compare: true,
    headroom: 0.15,
    samplesInformational: true,
  },
  'appservice-gridview': {
    note: 'official engine requires a grid builder callback; synthetic ops leave the grid empty',
    compare: false,
  },
  'appservice-listview': {
    note: 'official engine requires a list builder callback; synthetic ops leave the list empty',
    compare: false,
  },
  // D2 feature probes.  The fixed CSS/layout behaviour now matches; the
  // remaining ratios are text rasterization or Skyline breakAll metrics.
  'appservice-vertical-align': {
    note: 'inline-block line layout now matches; residual is text rasterization/metrics (~0.8%)',
    compare: true,
    headroom: 0.005,
    samplesInformational: true,
  },
  'appservice-text-overflow': {
    note: 'official clips at the same point in this op stream (no ellipsis); residual is text rasterization (~1.6%)',
    compare: true,
    headroom: 0.005,
    samplesInformational: true,
  },
  'appservice-white-space': {
    note: 'nowrap/normal switching now matches; residual is Skyline breakAll vs Flutter word wrapping (~2.3%)',
    compare: true,
    headroom: 0.005,
    samplesInformational: true,
  },
};

function missingPrerequisites() {
  const missing = [];
  if (!fs.existsSync(appDir)) missing.push('tools/l5-win-app');
  if (!windowsElectron()) missing.push('official Windows electron.exe');
  if (!windowsResources()) missing.push('official app.asar.unpacked resources');
  if (spawnSync(wineBinary(), ['--version']).status !== 0) missing.push('wine');
  return missing;
}

function winePath(value) {
  return 'Z:' + path.resolve(value).replace(/\//g, '\\');
}

function ensureShim(electron) {
  const source = path.dirname(electron);
  const expectedApp = path.join(appDir);
  const appLink = path.join(shimDir, 'resources', 'app');
  let current = null;
  try { current = fs.realpathSync(appLink); } catch {}
  if (fs.existsSync(path.join(shimDir, path.basename(electron)))
      && current === fs.realpathSync(expectedApp)) {
    return path.join(shimDir, path.basename(electron));
  }
  fs.rmSync(shimDir, { recursive: true, force: true });
  fs.mkdirSync(path.join(shimDir, 'resources'), { recursive: true });
  for (const entry of fs.readdirSync(source)) {
    if (entry === 'resources') continue;
    const from = path.join(source, entry);
    const to = path.join(shimDir, entry);
    try { fs.linkSync(from, to); } catch { fs.cpSync(from, to, { recursive: true }); }
  }
  fs.symlinkSync(appDir, appLink);
  return path.join(shimDir, path.basename(electron));
}

function samples(pixels) {
  const out = [];
  for (let y = 20; y < FRAME_HEIGHT; y += 40) {
    for (let x = 20; x < FRAME_WIDTH; x += 40) {
      if (IGNORE_RECTS.some(([x0, y0, x1, y1]) => x >= x0 && x < x1 && y >= y0 && y < y1)) {
        continue;
      }
      const offset = (y * FRAME_WIDTH + x) * 4;
      out.push([x, y, Array.from(pixels.subarray(offset, offset + 4))]);
    }
  }
  return out;
}

function nonWhite(pixels) {
  let n = 0;
  for (let i = 0; i + 3 < pixels.length; i += 4 * 97) {
    if (pixels[i] !== 255 || pixels[i + 1] !== 255 || pixels[i + 2] !== 255) n++;
  }
  return n;
}

function captureOne(name, electron, resources) {
  const fixture = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
  if (!fs.existsSync(fixture)) throw new Error(`missing fixture ${fixture}`);
  const frameFile = path.join(frameDir, `${name}.rgba`);
  const reportFile = path.join(frameDir, `${name}.json`);
  fs.rmSync(frameFile, { force: true });
  fs.rmSync(reportFile, { force: true });
  const env = {
    ...process.env,
    SKYLINE_WIN_RESOURCES: resources,
    L5_FIXTURE: winePath(fixture),
    L5_FRAME: winePath(frameFile),
    L5_OUT: winePath(reportFile),
    L5_KEY: `l5win_${name}_${process.pid}_${Date.now().toString(36)}`,
    L5_WIDTH: String(LOGICAL_WIDTH),
    L5_HEIGHT: String(LOGICAL_HEIGHT),
    L5_DPR: String(DPR),
    WINEDEBUG: '-all',
  };
  const started = Date.now();
  const proc = spawnSync(wineBinary(), [electron, '--no-sandbox'], {
    // Electron leaves GPU/utility children alive briefly; do not wait on
    // inherited stdio pipes when the frame/report files already exist.
    cwd: path.dirname(electron), env, encoding: 'utf8',
    stdio: 'ignore', timeout: 45000,
  });
  const elapsed = Date.now() - started;
  if (!fs.existsSync(frameFile)) {
    let events = '';
    try {
      const report = JSON.parse(fs.readFileSync(reportFile, 'utf8'));
      events = (report.events || []).slice(-12)
        .map((e) => e.line || e.kind || e.message || '').join('\n');
    } catch {}
    throw new Error(`no frame for ${name} (${elapsed}ms, exit=${proc.status})\n${events}`);
  }
  const frame = fs.readFileSync(frameFile);
  if (frame.length !== FRAME_SIZE) {
    throw new Error(`bad ${name} frame size ${frame.length} != ${FRAME_SIZE}`);
  }
  // Official Windows shared memory is BGRA; store canonical RGBA.
  for (let i = 0; i + 3 < frame.length; i += 4) {
    const blue = frame[i];
    frame[i] = frame[i + 2];
    frame[i + 2] = blue;
  }
  fs.writeFileSync(frameFile, frame);
  let report = null;
  try { report = JSON.parse(fs.readFileSync(reportFile, 'utf8')); } catch {}
  return { frame, elapsed, report };
}

function measuredStats(win, linux) {
  const result = compareFrames(win, linux, {
    width: FRAME_WIDTH,
    tolerance: TOLERANCE,
    ignoreRects: IGNORE_RECTS,
    maxMismatchRatio: 1,
  });
  return result;
}

function writeBaseline(electron, resources, reuse, only = null) {
  fs.mkdirSync(frameDir, { recursive: true });
  const selected = only
    ? l5.FIXTURES.filter((file) => only.includes(file.replace(/\.ops\.json$/, '')))
    : l5.FIXTURES;
  if (selected.length === 0) {
    throw new Error(`--only matched no fixtures: ${(only || []).join(',')}`);
  }
  // `--only` is a merge mode: keep the committed frames for untouched
  // fixtures so adding a probe does not recapture (and churn) the baseline.
  const existing = only && fs.existsSync(baselinePath)
    ? JSON.parse(fs.readFileSync(baselinePath, 'utf8'))
    : null;
  const fixtures = existing && existing.fixtures ? { ...existing.fixtures } : {};
  let skyline = existing && existing.skyline ? existing.skyline : null;
  for (const file of selected) {
    const name = file.replace(/\.ops\.json$/, '');
    const frameFile = path.join(frameDir, `${name}.rgba`);
    let frame;
    let report = null;
    if (reuse && fs.existsSync(frameFile)) {
      frame = fs.readFileSync(frameFile);
      try { report = JSON.parse(fs.readFileSync(path.join(frameDir, `${name}.json`), 'utf8')); } catch {}
      console.error(`reuse ${name}`);
    } else {
      const captured = captureOne(name, electron, resources);
      frame = captured.frame;
      report = captured.report;
      console.error(`captured ${name} in ${captured.elapsed}ms`);
    }
    const capture = report && report.report ? report.report : report;
    if (capture && capture.skyline) skyline = capture.skyline;
    const divergence = KNOWN_DIVERGENCES[name];
    const entry = {
      sha256: crypto.createHash('sha256').update(frame).digest('hex'),
      nonWhite: nonWhite(frame),
      samples: samples(frame),
      rgbaZlib: zlib.deflateSync(frame, { level: 9 }).toString('base64'),
    };
    if (divergence) {
      entry.note = divergence.note;
      entry.compare = divergence.compare !== false;
      if (divergence.samplesInformational) entry.samplesInformational = true;
    }
    if (!divergence || divergence.compare !== false) {
      const fixture = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
      let linux = null;
      try {
        linux = l5.render(fixture, { dpr: DPR, width: LOGICAL_WIDTH, height: LOGICAL_HEIGHT });
      } catch (error) {
        console.error(`warning: cannot render Linux ${name}: ${error.message}`);
      }
      if (linux) {
        const stats = measuredStats(frame, linux);
        entry.maxDelta = stats.maxDelta;
        entry.mismatchRatio = Number(stats.mismatchRatio.toFixed(6));
        if (stats.maxDelta > TOLERANCE) entry.samplesInformational = true;
        const headroom = divergence ? (divergence.headroom || 0.02) : 0.01;
        entry.maxMismatchRatio = Number(Math.min(1, stats.mismatchRatio + headroom).toFixed(6));
        console.error(`  compare ${name}: maxDelta=${stats.maxDelta} ratio=${stats.mismatchRatio.toFixed(6)}`);
      }
    }
    fixtures[name] = entry;
  }
  const baseline = {
    comment: existing && existing.comment
      ? existing.comment
      : 'Official Windows Skyline engine frames captured under Wine (Electron renderer, ' +
        'dpr=2). Raw shared memory is BGRA on Windows and is stored here as RGBA. ' +
        'Coordinates are device pixels. The debug banner of the Linux comparison ' +
        'build is excluded via ignoreRects.',
    source: existing && existing.source ? existing.source : 'official-wechat-devtools-win32-x64',
    captured_at: new Date().toISOString().slice(0, 10),
    skyline: skyline || { skyline_version: '1.4.21' },
    width: existing && existing.width ? existing.width : LOGICAL_WIDTH,
    height: existing && existing.height ? existing.height : LOGICAL_HEIGHT,
    dpr: existing && existing.dpr ? existing.dpr : DPR,
    frame_width: existing && existing.frame_width ? existing.frame_width : FRAME_WIDTH,
    frame_height: existing && existing.frame_height ? existing.frame_height : FRAME_HEIGHT,
    tolerance: existing && Number.isInteger(existing.tolerance) ? existing.tolerance : TOLERANCE,
    ignoreRects: existing && existing.ignoreRects ? existing.ignoreRects : IGNORE_RECTS,
    fixtures,
  };
  fs.writeFileSync(baselinePath, JSON.stringify(baseline, null, 1) + '\n');
  console.error(`wrote ${baselinePath}`);
}

function parseOnly(argv) {
  let raw = null;
  const index = argv.indexOf('--only');
  if (index >= 0) raw = argv[index + 1] || '';
  const inline = argv.find((arg) => arg.startsWith('--only='));
  if (inline) raw = inline.slice('--only='.length);
  if (!raw) return null;
  return raw.split(',').map((name) => name.trim()).filter(Boolean)
    .map((name) => name.replace(/\.ops\.json$/, ''));
}

function compareBaseline() {
  if (!fs.existsSync(baselinePath)) {
    console.error(`missing ${baselinePath}; run with --write`);
    process.exit(1);
  }
  const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));
  const dpr = baseline.dpr || DPR;
  let failures = 0;
  let compared = 0;
  let skipped = 0;
  for (const [name, expected] of Object.entries(baseline.fixtures || {})) {
    const fixture = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
    if (!fs.existsSync(fixture)) { console.error(`skip ${name}: fixture missing`); continue; }
    if (expected.compare === false) {
      skipped++;
      console.log(`skip ${name}: ${expected.note || 'not comparable'}`);
      continue;
    }
    const win = zlib.inflateSync(Buffer.from(expected.rgbaZlib, 'base64'));
    const linux = l5.render(fixture, {
      dpr, width: baseline.width, height: baseline.height,
    });
    const tolerance = Number.isInteger(expected.tolerance)
      ? expected.tolerance : baseline.tolerance;
    const maxRatio = typeof expected.maxMismatchRatio === 'number'
      ? expected.maxMismatchRatio : baseline.maxMismatchRatio ?? 0.01;
    const result = compareFrames(win, linux, {
      width: baseline.frame_width || FRAME_WIDTH,
      tolerance,
      maxMismatchRatio: maxRatio,
      ignoreRects: baseline.ignoreRects || IGNORE_RECTS,
    });
    compared++;
    const status = result.passed ? 'ok  ' : 'FAIL';
    console.log(`${status} ${name} maxDelta=${result.maxDelta} ` +
      `mismatchRatio=${result.mismatchRatio.toFixed(6)} (<= ${maxRatio})`);
    if (!result.passed) failures++;
  }
  if (compared === 0) {
    console.error('baseline has no comparable fixtures');
    failures++;
  }
  console.log(`${compared} compared, ${skipped} skipped, ${failures} failed`);
  process.exit(failures === 0 ? 0 : 1);
}

function main() {
  const missing = missingPrerequisites();
  if (missing.length) {
    console.error(`missing prerequisites: ${missing.join(', ')}`);
    process.exit(77);
  }
  const electron = ensureShim(windowsElectron());
  const resources = windowsResources();
  if (process.argv.includes('--write')) {
    writeBaseline(electron, resources, process.argv.includes('--reuse'), parseOnly(process.argv));
    return;
  }
  compareBaseline();
}

if (require.main === module) main();

module.exports = {
  compareBaseline, writeBaseline, captureOne, ensureShim, missingPrerequisites,
  IGNORE_RECTS, FRAME_WIDTH, FRAME_HEIGHT,
};

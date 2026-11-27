'use strict';

// Records/compares the L5 Linux frame baseline.
//
//   node tools/l5-baseline.js --write
//   node tools/l5-baseline.js            # compare current frames to the file
//
// The official Windows appservice guest is still unavailable (see
// docs/feasibility.md section 3).  This tool produces the Linux half of the
// comparison and defines the schema consumed by
// native/test/l5-pixel-compare.test.js when a Windows baseline is supplied
// (`spec/l5-baseline.win32.json`).

const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { devtoolsEntrypointArgs } = require('../test/lib/host');

const repoRoot = path.resolve(__dirname, '..');
const host = path.join(repoRoot, 'native', 'build', 'skyline_host');
const engineDir = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-debug');
const engineLib = path.join(engineDir, 'libflutter_engine.so');
const engineRuntime = path.join(engineDir, 'libskyline.so');
const engineIcu = path.join(engineDir, 'icudtl.dat');
const toolchainIcu = path.join(
  repoRoot, 'native', 'engine', 'toolchain', 'engine-artifacts', 'icudtl.dat',
);
const icu = fs.existsSync(engineIcu) ? engineIcu : toolchainIcu;
const assets = path.join(
  repoRoot, 'native', 'engine', 'work', 'skyline-app', 'build', 'flutter_assets',
);
const kernel = path.join(assets, 'kernel_blob.bin');
const baselinePath = path.join(repoRoot, 'spec', 'l5-baseline.json');

const logicalWidth = 320;
const logicalHeight = 240;
const width = logicalWidth;
const height = logicalHeight;
const frameSize = width * height * 4;

const FIXTURES = [
  'appservice-stripes.ops.json',
  'appservice-fixed.ops.json',
  'appservice-transform3d.ops.json',
  'appservice-transform-origin.ops.json',
  'appservice-align-gap.ops.json',
  'appservice-flex-wrap.ops.json',
  'appservice-order.ops.json',
  'appservice-overflow.ops.json',
  'appservice-fixed-scroll.ops.json',
  'appservice-absolute.ops.json',
  'appservice-absolute-margin.ops.json',
  'appservice-text-padding.ops.json',
  'appservice-style-scope.ops.json',
  'appservice-appbar.ops.json',
  'appservice-content-box.ops.json',
  'appservice-content-box-shrink.ops.json',
  'appservice-display-flex.ops.json',
  'appservice-calc-env.ops.json',
  'appservice-content-box-nav.ops.json',
  'appservice-content-box-stretch.ops.json',
  'appservice-content-box-clamp.ops.json',
  'appservice-content-box-clamp2.ops.json',
  'appservice-shadow.ops.json',
  'appservice-gridview.ops.json',
  'appservice-listview.ops.json',
  'appservice-flex-basis.ops.json',
  'appservice-box-sizing.ops.json',
  'appservice-percent-box.ops.json',
  'appservice-sticky.ops.json',
  'appservice-radial-conic.ops.json',
  'appservice-background-size.ops.json',
  'appservice-transform-origin-z.ops.json',
  // D2 feature probes (candidate CSS/builtin gaps vs the Windows oracle).
  'appservice-aspect-ratio.ops.json',
  'appservice-vertical-align.ops.json',
  'appservice-text-overflow.ops.json',
  'appservice-white-space.ops.json',
  'appservice-map.ops.json',
  // D3 probes: scroll-view direction, swiper `height:auto` base-sheet
  // override, and bold inline text metrics (associated-scroll-view page).
  'appservice-scroll-horizontal.ops.json',
  'appservice-swiper-auto-height.ops.json',
  'appservice-inline-bold-price.ops.json',
];

// Fixtures in test/fixtures that are deliberately not part of L5.  Every
// fixture must be in exactly one of FIXTURES and EXCLUDED_FIXTURES, and both
// baselines must cover FIXTURES exactly (see fixtureSetProblems()).
const EXCLUDED_FIXTURES = {
  // Static pages that could join L5 once a Windows frame is captured
  // (npm run l5:windows -- --only <name> --write).
  'appservice-inline-text-wrap.ops.json': 'static page; no Windows frame captured yet',
  'appservice-scroll-static.ops.json': 'static page; no Windows frame captured yet',
  'appservice-transform.ops.json': 'static page; no Windows frame captured yet',
  // Timed `updates` batches or animations: the final frame depends on the
  // replay clock rather than the page content.
  'appservice-animated-style.ops.json': 'addAnimatedStyle timeline; frame depends on the replay clock',
  'appservice-builder-update.ops.json': 'timed updates batches',
  'appservice-native-view-release.ops.json': 'timed updates batches (texture dispose)',
  'appservice-route.ops.json': 'timed updates batches (route push/pop)',
  'appservice-scroll.ops.json': 'timed updates batches (programmatic scroll)',
  'appservice-swiper.ops.json': 'timed updates batches (swiper page animation)',
  'appservice-text-update.ops.json': 'timed updates batches',
  'appservice-transition.ops.json': 'CSS transition; frame depends on the replay clock',
  'appservice-update.ops.json': 'timed updates batches',
  // Input-driven behaviour probes (pointer scripts).
  'appservice-fixed-overlay-scroll.ops.json': 'touch hit-test probe driven by a pointer script',
  'appservice-pointer-events.ops.json': 'touch hit-test probe driven by a pointer script',
  'appservice-pointer-events-control.ops.json': 'touch hit-test probe driven by a pointer script',
  'appservice-swiper-scroll-yield.ops.json': 'gesture arbitration probe driven by a pointer script',
  // Builder callbacks / external resources / native content.
  'appservice-builder.ops.json': 'builder callbacks; synthetic ops do not drive the official builder (as gridview/listview)',
  'appservice-font.ops.json': 'needs @font-face files from SKYLINE_RESOURCE_ROOT',
  'appservice-image.ops.json': 'needs a generated bitmap from SKYLINE_RESOURCE_ROOT',
  'appservice-native-view.ops.json': 'native view texture; Linux paints a placeholder slot',
};

const fixturesDir = path.join(repoRoot, 'test', 'fixtures');

function fixtureKey(name) {
  return name.replace(/\.ops\.json$/, '');
}

// Returns human-readable problems; empty when the fixture directory, the
// FIXTURES/EXCLUDED_FIXTURES lists and the given baselines agree.  Pass
// `baselines` as `{ label: parsedBaselineJson }`.
function fixtureSetProblems(baselines = {}) {
  const problems = [];
  const onDisk = new Set(fs.readdirSync(fixturesDir).filter(name => name.endsWith('.ops.json')));
  const listed = new Set();
  for (const name of FIXTURES) {
    if (listed.has(name)) problems.push(`${name} is listed twice in FIXTURES`);
    listed.add(name);
    if (name in EXCLUDED_FIXTURES) problems.push(`${name} is both in FIXTURES and EXCLUDED_FIXTURES`);
  }
  for (const name of [...FIXTURES, ...Object.keys(EXCLUDED_FIXTURES)]) {
    if (!onDisk.has(name)) problems.push(`${name} does not exist in test/fixtures`);
  }
  for (const name of onDisk) {
    if (!listed.has(name) && !(name in EXCLUDED_FIXTURES)) {
      problems.push(`${name} is in neither FIXTURES nor EXCLUDED_FIXTURES (tools/l5-baseline.js)`);
    }
  }
  const expected = new Set(FIXTURES.map(fixtureKey));
  for (const [label, baseline] of Object.entries(baselines)) {
    const keys = new Set(Object.keys((baseline && baseline.fixtures) || {}));
    for (const key of expected) {
      if (!keys.has(key)) problems.push(`${label} has no entry for ${key}`);
    }
    for (const key of keys) {
      if (!expected.has(key)) problems.push(`${label} has an entry for ${key}, which is not in FIXTURES`);
    }
  }
  return problems;
}

function prerequisites() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so');
  if (!fs.existsSync(engineRuntime)) missing.push('embedded libskyline.so');
  if (!fs.existsSync(kernel)) missing.push('kernel_blob.bin');
  if (!fs.existsSync(icu)) missing.push('icudtl.dat');
  return missing;
}

function render(fixturePath, options = {}) {
  const dpr = options.dpr === undefined ? 1 : options.dpr;
  const cssWidth = options.width === undefined ? logicalWidth : options.width;
  const cssHeight = options.height === undefined ? logicalHeight : options.height;
  const frameWidth = Math.ceil(cssWidth * dpr);
  const frameHeight = Math.ceil(cssHeight * dpr);
  const expectedFrameSize = frameWidth * frameHeight * 4;
  const key = `skyline_l5_${process.pid}_${Date.now().toString(36)}_${Math.random()
    .toString(36)
    .slice(2, 8)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  const env = {
    ...process.env,
    LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(':'),
    SKYLINE_APP_SERVICE_OPS: `@${fixturePath}`,
  };
  const result = spawnSync(host, [
    '--assets', assets,
    '--icu', icu,
    '--width', String(cssWidth),
    '--height', String(cssHeight),
    '--dpr', String(dpr),
    '--shm-key', key,
    '--timeout-ms', '1800',
    '--frames', '0',
    '--no-shutdown',
    ...devtoolsEntrypointArgs({ width: cssWidth, height: cssHeight, skylineLibrary: engineLib }),
  ], { encoding: 'utf8', timeout: 45000, env });
  try {
    if (!fs.existsSync(shmPath)) {
      throw new Error(`no frame for ${path.basename(fixturePath)}:\n${result.stderr}`);
    }
    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    if (buffer.length !== 16 + dataSize) {
      throw new Error(`bad shared-memory size for ${path.basename(fixturePath)}`);
    }
    const data = buffer.subarray(16);
    if (data.readUInt32LE(4) !== expectedFrameSize) {
      throw new Error(`bad frame size for ${path.basename(fixturePath)}`);
    }
    return data.subarray(8, 8 + expectedFrameSize);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Best effort.
    }
  }
}

function samples(pixels) {
  const out = [];
  for (let y = 10; y < height; y += 40) {
    for (let x = 10; x < width; x += 40) {
      const offset = (y * width + x) * 4;
      out.push([x, y, Array.from(pixels.subarray(offset, offset + 4))]);
    }
  }
  return out;
}

function record() {
  const fixtures = {};
  for (const name of FIXTURES) {
    const pixels = render(path.join(fixturesDir, name));
    fixtures[fixtureKey(name)] = {
      width,
      height,
      sha256: crypto.createHash('sha256').update(pixels).digest('hex'),
      samples: samples(pixels),
    };
    console.error(`recorded ${name} -> ${fixtures[fixtureKey(name)].sha256}`);
  }
  return {
    comment:
      'Linux-side deterministic frame baseline. Windows entries use the same schema.',
    fixtures,
  };
}

function compare(baseline) {
  let failed = 0;
  for (const [name, expected] of Object.entries(baseline.fixtures || {})) {
    const pixels = render(path.join(fixturesDir, `${name}.ops.json`));
    const hash = crypto.createHash('sha256').update(pixels).digest('hex');
    if (hash === expected.sha256) {
      console.log(`ok ${name} ${hash}`);
      continue;
    }
    failed++;
    let maxDelta = 0;
    let mismatched = 0;
    for (const [x, y, rgba] of expected.samples || []) {
      const offset = (y * width + x) * 4;
      for (let i = 0; i < 4; i++) {
        const delta = Math.abs(pixels[offset + i] - rgba[i]);
        if (delta > maxDelta) maxDelta = delta;
        if (delta > 0) mismatched++;
      }
    }
    console.error(
      `FAIL ${name}: expected ${expected.sha256}, got ${hash} ` +
      `(maxDelta=${maxDelta}, mismatchedChannels=${mismatched})`);
  }
  return failed === 0;
}

function main() {
  const missing = prerequisites();
  if (missing.length > 0) {
    console.error(`missing prerequisites: ${missing.join(', ')}`);
    process.exit(77);
  }
  const listProblems = fixtureSetProblems();
  if (listProblems.length > 0) {
    console.error(`fixture lists are inconsistent:\n  ${listProblems.join('\n  ')}`);
    process.exit(1);
  }
  if (process.argv.includes('--write')) {
    const baseline = record();
    fs.writeFileSync(baselinePath, JSON.stringify(baseline, null, 1) + '\n');
    console.error(`wrote ${baselinePath}`);
    return;
  }
  if (!fs.existsSync(baselinePath)) {
    console.error(`missing ${baselinePath}; run with --write`);
    process.exit(1);
  }
  const baseline = JSON.parse(fs.readFileSync(baselinePath, 'utf8'));
  const baselineProblems = fixtureSetProblems({ 'spec/l5-baseline.json': baseline });
  if (baselineProblems.length > 0) {
    console.error(`baseline does not match FIXTURES (run with --write):\n  ${baselineProblems.join('\n  ')}`);
    process.exit(1);
  }
  process.exit(compare(baseline) ? 0 : 1);
}

if (require.main === module) main();

module.exports = {
  EXCLUDED_FIXTURES,
  FIXTURES,
  baselinePath,
  fixtureKey,
  fixtureSetProblems,
  fixturesDir,
  frameSize,
  height,
  prerequisites,
  render,
  samples,
  width,
};

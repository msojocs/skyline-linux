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

const width = 320;
const height = 240;
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
  'appservice-shadow.ops.json',
  'appservice-gridview.ops.json',
  'appservice-listview.ops.json',
];

function prerequisites() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so');
  if (!fs.existsSync(engineRuntime)) missing.push('embedded libskyline.so');
  if (!fs.existsSync(kernel)) missing.push('kernel_blob.bin');
  if (!fs.existsSync(icu)) missing.push('icudtl.dat');
  return missing;
}

function render(fixturePath) {
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
    '--width', String(width),
    '--height', String(height),
    '--shm-key', key,
    '--timeout-ms', '1800',
    '--frames', '0',
    '--no-shutdown',
    '--entrypoint-arg', '1',
    '--entrypoint-arg', '1',
    '--entrypoint-arg', String(width),
    '--entrypoint-arg', String(height),
    '--entrypoint-arg', '1',
    '--entrypoint-arg', 'stub',
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
    if (data.readUInt32LE(4) !== frameSize) {
      throw new Error(`bad frame size for ${path.basename(fixturePath)}`);
    }
    return data.subarray(8, 8 + frameSize);
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
    const fixturePath = path.join(repoRoot, 'test', 'fixtures', name);
    if (!fs.existsSync(fixturePath)) continue;
    const pixels = render(fixturePath);
    fixtures[name.replace(/\.ops\.json$/, '')] = {
      width,
      height,
      sha256: crypto.createHash('sha256').update(pixels).digest('hex'),
      samples: samples(pixels),
    };
    console.error(`recorded ${name} -> ${fixtures[name.replace(/\.ops\.json$/, '')].sha256}`);
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
    const fixturePath = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
    if (!fs.existsSync(fixturePath)) {
      console.error(`skip ${name}: fixture missing`);
      continue;
    }
    const pixels = render(fixturePath);
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
  process.exit(compare(baseline) ? 0 : 1);
}

if (require.main === module) main();

module.exports = {
  FIXTURES,
  baselinePath,
  frameSize,
  height,
  prerequisites,
  render,
  samples,
  width,
};

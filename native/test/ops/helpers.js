'use strict';

// Shared helpers for the appservice op-replay tests (native/test/ops/*.test.js).
//
// `tools/record-skyline-ops.js` runs the official Skyline renderer bundle
// (exparser + skyline-js) in Node's V8 and captures the node-operation stream
// that the bundle would send to `skyline.node`'s native bindings.  The embedded
// runtime (native/runtime/appservice_*.cc) replays that stream through the real
// `package:skyline` callbacks.  Each test file covers one area; all of them
// launch skyline_host through test/lib/host.js.

const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const {
  SKYLINE_DEBUG,
  checkBuildCurrent,
  countPixels,
  pixelAt: framePixelAt,
  prerequisiteSkip,
  runHost,
} = require('../../../test/lib/host');
const { windowsResourceCandidates } = require('../../../test/lib/env');

const repoRoot = path.resolve(__dirname, '..', '..', '..');
const recorder = path.join(repoRoot, 'tools', 'record-skyline-ops.js');

const width = 320;
const height = 240;

// node:test `skip` value shared by every op-replay test.
const opsSkip = prerequisiteSkip(SKYLINE_DEBUG);

// Registers the build-freshness check for one test file.
function checkOpsBuild(prefix) {
  test(`${prefix}: build artifacts match the sources`, { skip: opsSkip }, t => {
    checkBuildCurrent(t, ['engine-debug', 'kernel']);
  });
}

// Replays `opsPath` at 320x240 (`options.dpr` scales the frame) and returns
// `{ result, logs, details, pixels }`.
function runOps(opsPath, options = {}) {
  return runHost({
    key: 'skyline_ops',
    ops: opsPath,
    width,
    height,
    dpr: options.dpr || 1,
    timeoutMs: options.timeoutMs || 20000,
    frames: options.frames,
    args: options.args,
    env: options.env,
  });
}

function pixelAt(pixels, x, y) {
  return framePixelAt(pixels, width, x, y);
}

function countColor(pixels, predicate) {
  return countPixels(pixels, predicate);
}

function writeQuadBmp(file) {
  const width = 8;
  const height = 8;
  const rowBytes = width * 3;
  const pixels = Buffer.alloc(rowBytes * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const top = y < height / 2;
      const left = x < width / 2;
      let r = 0;
      let g = 0;
      let b = 0;
      if (top && left) {
        r = 255;
      } else if (top) {
        g = 255;
      } else if (left) {
        b = 255;
      } else {
        r = 255;
        g = 255;
      }
      const offset = (height - 1 - y) * rowBytes + x * 3;
      pixels[offset] = b;
      pixels[offset + 1] = g;
      pixels[offset + 2] = r;
    }
  }
  const header = Buffer.alloc(54);
  header.write('BM', 0);
  header.writeUInt32LE(54 + pixels.length, 2);
  header.writeUInt32LE(54, 10);
  header.writeUInt32LE(40, 14);
  header.writeInt32LE(width, 18);
  header.writeInt32LE(height, 22);
  header.writeUInt16LE(1, 26);
  header.writeUInt16LE(24, 28);
  header.writeUInt32LE(0, 30);
  header.writeUInt32LE(pixels.length, 34);
  fs.writeFileSync(file, Buffer.concat([header, pixels]));
}

function systemFontPath() {
  const candidates = [
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
    '/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
  ];
  return candidates.find((candidate) => fs.existsSync(candidate)) || null;
}

function recordRealOps(options = {}) {
  if (!fs.existsSync(recorder)) return null;
  const suffix = options.tapOption === undefined ? '' : `-tap-${options.tapOption}`;
  const textSuffix = options.tapText === undefined ? '' : '-tap-text';
  const out = path.join(
    os.tmpdir(), `skyline-ops-${process.pid}${suffix}${textSuffix}.json`);
  const args = [recorder, '--out', out, '--quiet'];
  if (options.tapOption !== undefined) {
    args.push('--tap-option', String(options.tapOption));
  }
  if (options.tapText !== undefined) {
    args.push('--tap-text', String(options.tapText));
  }
  if (options.bundle !== undefined) {
    args.push('--bundle', options.bundle);
  }
  if (options.animateMs !== undefined) {
    args.push('--animate-ms', String(options.animateMs));
  }
  const result = spawnSync(process.execPath, args, {
    encoding: 'utf8',
    timeout: 30000,
  });
  if (result.status !== 0 || !fs.existsSync(out)) return null;
  try {
    return { path: out, payload: JSON.parse(fs.readFileSync(out, 'utf8')) };
  } catch {
    return null;
  }
}

function legacyBundle() {
  for (const resource of windowsResourceCandidates()) {
    const candidate = path.join(resource, 'node_modules', 'skyline-addon',
      'bundle', 'flutter_assets', 'assets', 'app_0.js');
    if (fs.existsSync(candidate)) return candidate;
  }
  return null;
}

module.exports = {
  checkOpsBuild,
  countColor,
  height,
  legacyBundle,
  opsSkip,
  pixelAt,
  recordRealOps,
  repoRoot,
  runOps,
  systemFontPath,
  width,
  writeQuadBmp,
};

'use strict';

// Phase-2 engine smoke test: boot the Linux Flutter engine with the minimal
// dart:ui page, capture one software-rendered frame, and validate the frame written
// to /dev/shm/skyline_<key>.dat (docs/contract.md section 4).
//
// The test skips when the (multi-GiB) engine build has not been staged; the host
// stub returns exit 77 in that case.  Set SKYLINE_REQUIRE_ENGINE=1 in CI once the
// engine is built to turn the skip into a failure.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build', 'skyline_host');
const kernel = path.join(repoRoot, 'apps', 'empty-page', 'build', 'flutter_assets', 'kernel_blob.bin');
const engineDir = path.join(repoRoot, 'native', 'engine');
const engineIcu = path.join(engineDir, 'out', 'linux-x64-debug', 'icudtl.dat');
const toolchainIcu = path.join(
  engineDir,
  'toolchain',
  'engine-artifacts',
  'icudtl.dat',
);
const icu = fs.existsSync(engineIcu) ? engineIcu : toolchainIcu;
const engineLib = path.join(engineDir, 'out', 'linux-x64-debug', 'libflutter_engine.so');

const width = 64;
const height = 48;
const dpr = 2;
const physicalWidth = Math.ceil(width * dpr);
const physicalHeight = Math.ceil(height * dpr);
const expectedFrameSize = physicalWidth * physicalHeight * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host (npm run native:build)');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so (bash native/engine/fetch-engine.sh && bash native/engine/build-engine.sh)');
  if (!fs.existsSync(kernel)) missing.push('apps/empty-page/build/flutter_assets/kernel_blob.bin (npm run app:build)');
  if (!fs.existsSync(icu)) missing.push('icudtl.dat (engine build or npm run toolchain:fetch)');
  return missing.length ? missing.join('; ') : null;
}

function parseResult(stdout) {
  const match = /^SKYLINE_HOST_RESULT (.*)$/m.exec(stdout || '');
  if (!match) return null;
  try {
    return JSON.parse(match[1]);
  } catch {
    return null;
  }
}

test('engine smoke: one software-rendered frame reaches shared memory', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `engine_smoke_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  try {
    const result = spawnSync(host, [
      '--assets', path.dirname(kernel),
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
      '--dpr', String(dpr),
      '--shm-key', key,
      '--timeout-ms', '20000',
    ], { encoding: 'utf8', timeout: 30000 });

    const details = parseResult(result.stdout);
    const diagnostics = [
      result.stdout && result.stdout.trim(),
      result.stderr && result.stderr.trim(),
    ].filter(Boolean).join('\n');

    if (result.status === 77) {
      if (process.env.SKYLINE_REQUIRE_ENGINE === '1') {
        assert.fail(`engine is required but not built:\n${diagnostics}`);
      }
      // The stub's message tells the operator how to fetch/build the engine.
      return;
    }

    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.equal(details.width, physicalWidth);
    assert.equal(details.height, physicalHeight);
    assert.equal(details.frameSize, expectedFrameSize);
    assert.ok(details.vsyncCount >= 1, 'the app never requested a vsync');

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.readInt32LE(8), 1, 'outer header version must be 1');
    assert.equal(buffer.readUInt32LE(12), 0, 'outer header padding must be zero');
    assert.equal(buffer.length, 16 + dataSize, 'file length must match the outer header');

    const data = buffer.subarray(16);
    const frameId = data.readUInt32LE(0);
    const frameSize = data.readUInt32LE(4);
    assert.ok(frameId >= 1, 'frameId must be positive');
    assert.equal(frameSize, expectedFrameSize);
    assert.ok(data.length >= 8 + frameSize, 'data region is too small for the frame');

    // The page is opaque red.  The host converts Skia N32/BGRA to RGBA.
    const pixels = data.subarray(8, 8 + frameSize);
    assert.deepEqual([...pixels.subarray(0, 4)], [255, 0, 0, 255], 'first pixel must be opaque red');
    const center = (Math.floor(physicalHeight / 2) * physicalWidth + Math.floor(physicalWidth / 2)) * 4;
    assert.deepEqual([...pixels.subarray(center, center + 4)], [255, 0, 0, 255], 'center pixel must be opaque red');
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine: the test may have skipped.
    }
  }
});

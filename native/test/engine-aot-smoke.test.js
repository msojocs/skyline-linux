'use strict';

// Phase-2 AOT path: a release Flutter engine launched with `libapp.so`
// (`FlutterEngineCreateAOTData`) instead of a JIT kernel blob.
//
// Prerequisites (documented in native/engine/README.md):
//   npm run engine:build -- --runtime-mode release   # native/engine/out/linux-x64-release
//   npm run app:aot                                  # apps/empty-page/build/libapp.so
//   cmake -S native -B native/build-release \
//     -DSKYLINE_ENGINE_ROOT=$PWD/native/engine/out/linux-x64-release
//   cmake --build native/build-release --target skyline_host
//
// Skips when the release host/engine/snapshot are not present.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build-release', 'skyline_host');
const engineDir = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-release');
const engineLib = path.join(engineDir, 'libflutter_engine.so');
const icu = path.join(engineDir, 'icudtl.dat');
const bundle = path.join(repoRoot, 'apps', 'empty-page', 'build');
const assets = path.join(bundle, 'flutter_assets');
const aot = path.join(bundle, 'libapp.so');

const width = 128;
const height = 96;
const frameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) {
    missing.push('native/build-release/skyline_host (release engine build)');
  }
  if (!fs.existsSync(engineLib)) {
    missing.push('native/engine/out/linux-x64-release/libflutter_engine.so');
  }
  if (!fs.existsSync(icu)) missing.push('release icudtl.dat');
  if (!fs.existsSync(aot)) missing.push('apps/empty-page/build/libapp.so (npm run app:aot)');
  return missing.length ? missing.join('; ') : null;
}

test('engine aot: release host renders a frame from libapp.so', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_aot_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--aot-library', aot,
      '--width', String(width),
      '--height', String(height),
      '--dpr', '1',
      '--shm-key', key,
      '--timeout-ms', '15000',
      '--no-shutdown',
    ], {
      encoding: 'utf8',
      timeout: 40000,
      env: {
        ...process.env,
        LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH]
          .filter(Boolean).join(':'),
      },
    });
    const logs = [result.stdout, result.stderr].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `release host failed:\n${logs}`);
    assert.match(logs, /\[host\] AOT snapshot: .*libapp\.so/);
    assert.match(logs, /SKYLINE_HOST_RESULT/);

    const frame = fs.readFileSync(shmPath);
    const dataSize = Number(frame.readBigUInt64LE(0));
    assert.equal(frame.length, 16 + dataSize);
    const data = frame.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    assert.deepEqual([...pixels.subarray(0, 4)], [255, 0, 0, 255]);
    assert.deepEqual([...pixels.subarray(frameSize - 4)], [255, 0, 0, 255]);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

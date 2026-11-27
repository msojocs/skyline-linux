'use strict';

// Phase-2/3 integration: the recompiled `package:skyline` appservice running as
// an AOT snapshot on the release engine with the embedded Skyline runtime.
//
// Prerequisites:
//   npm run engine:build -- --runtime-mode release
//   npm run app:aot -- --app-dir native/engine/work/skyline-app \
//     --entry native/engine/work/skyline-app/packages/sample/lib/devtools.dart \
//     --patched-sdk
//   npm run runtime:embed -- --runtime-mode release
//   cmake -S native -B native/build-release \
//     -DSKYLINE_ENGINE_ROOT=$PWD/native/engine/out/linux-x64-release
//   cmake --build native/build-release --target skyline_host
//
// Skips when the AOT/release artifacts are not present.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { devtoolsEntrypointArgs } = require('../../test/lib/host');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build-release', 'skyline_host');
const stage = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-release');
const engineLib = path.join(stage, 'libflutter_engine.so');
const runtimeLink = path.join(stage, 'libskyline.so');
const icu = path.join(stage, 'icudtl.dat');
const assets = path.join(
  repoRoot, 'native', 'engine', 'work', 'skyline-app', 'build', 'flutter_assets',
);
const aot = path.join(
  repoRoot, 'native', 'engine', 'work', 'skyline-app', 'build', 'libapp.so',
);
const fixture = path.join(
  repoRoot, 'test', 'fixtures', 'appservice-stripes.ops.json',
);

const width = 320;
const height = 240;
const frameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) {
    missing.push('native/build-release/skyline_host (release host build)');
  }
  if (!fs.existsSync(engineLib)) {
    missing.push('out/linux-x64-release/libflutter_engine.so');
  }
  if (!fs.existsSync(runtimeLink)) {
    missing.push('release runtime (npm run runtime:embed -- --runtime-mode release)');
  }
  if (!fs.existsSync(icu)) missing.push('release icudtl.dat');
  if (!fs.existsSync(aot)) missing.push('Skyline appservice AOT libapp.so');
  return missing.length ? missing.join('; ') : null;
}

function pixelAt(pixels, x, y) {
  const offset = (y * width + x) * 4;
  return [...pixels.subarray(offset, offset + 4)];
}

test('skyline aot: appservice stripes render on the release engine', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `skyline_aot_ops_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--aot-library', aot,
      '--width', String(width),
      '--height', String(height),
      '--dpr', '1',
      '--frames', '0',
      '--timeout-ms', '2500',
      '--shm-key', key,
      '--no-shutdown',
      ...devtoolsEntrypointArgs({ width, height, skylineLibrary: engineLib }),
    ], {
      encoding: 'utf8',
      timeout: 40000,
      env: {
        ...process.env,
        LD_LIBRARY_PATH: [stage, process.env.LD_LIBRARY_PATH]
          .filter(Boolean).join(':'),
        SKYLINE_APP_SERVICE_OPS: `@${fixture}`,
      },
    });
    const logs = [result.stdout, result.stderr].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `AOT appservice host failed:\n${logs}`);
    assert.match(logs, /\[host\] AOT snapshot: .*libapp\.so/);
    assert.match(logs, /devtools\.dart main args/);
    assert.match(logs, /appservice ops: 17 ops, 5 nodes -> CreatePageRoute/);

    const frame = fs.readFileSync(shmPath);
    const dataSize = Number(frame.readBigUInt64LE(0));
    assert.equal(frame.length, 16 + dataSize);
    const data = frame.subarray(16);
    assert.equal(data.readUInt32LE(4), frameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    // Same three stripes as the JIT replay fixture (see appservice-ops tests).
    assert.deepEqual(pixelAt(pixels, 10, 10), [255, 51, 102, 255]);
    assert.deepEqual(pixelAt(pixels, 10, 100), [51, 204, 102, 255]);
    assert.deepEqual(pixelAt(pixels, 10, 200), [51, 102, 255, 255]);
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

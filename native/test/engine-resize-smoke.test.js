'use strict';

// Phase-2 viewport resize: FlutterEngineSendWindowMetricsEvent updates the
// implicit view and subsequent frames use the new physical size.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build', 'skyline_host');
const engineDir = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-debug');
const engineLib = path.join(engineDir, 'libflutter_engine.so');
const icu = path.join(engineDir, 'icudtl.dat');
const assets = path.join(repoRoot, 'apps', 'empty-page', 'build', 'flutter_assets');
const kernel = path.join(assets, 'kernel_blob.bin');

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so');
  if (!fs.existsSync(icu)) missing.push('icudtl.dat');
  if (!fs.existsSync(kernel)) missing.push('empty-page kernel (npm run app:build)');
  return missing.length ? missing.join('; ') : null;
}

test('engine resize: viewport metrics update the physical frame size', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const script = path.join(os.tmpdir(), `skyline-resize-${process.pid}.txt`);
  fs.writeFileSync(script, '1000 64 64 1\n');
  const key = `skyline_resize_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  try {
    const result = spawnSync(host, [
      '--assets', assets,
      '--icu', icu,
      '--width', '128',
      '--height', '96',
      '--dpr', '1',
      '--frames', '0',
      '--timeout-ms', '2500',
      '--shm-key', key,
      '--no-shutdown',
      '--resize-script', script,
    ], {
      encoding: 'utf8',
      timeout: 30000,
      env: {
        ...process.env,
        LD_LIBRARY_PATH: [engineDir, process.env.LD_LIBRARY_PATH]
          .filter(Boolean).join(':'),
      },
    });
    const logs = [result.stdout, result.stderr].filter(Boolean).join('\n');
    assert.equal(result.status, 0, `resize host failed:\n${logs}`);
    assert.match(logs, /\[host\] resize: at=1000 64x64 dpr=1 -> 64x64/);
    const match = /SKYLINE_HOST_RESULT (.*)/.exec(result.stdout || '');
    assert.ok(match, `missing SKYLINE_HOST_RESULT:\n${logs}`);
    const details = JSON.parse(match[1]);
    assert.equal(details.width, 64);
    assert.equal(details.height, 64);
    assert.equal(details.frameSize, 64 * 64 * 4);

    const frame = fs.readFileSync(shmPath);
    const data = frame.subarray(16);
    assert.equal(data.readUInt32LE(4), 64 * 64 * 4);
    assert.deepEqual([...data.subarray(8, 12)], [255, 0, 0, 255]);
  } finally {
    try {
      fs.unlinkSync(script);
    } catch {
      // Best effort.
    }
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

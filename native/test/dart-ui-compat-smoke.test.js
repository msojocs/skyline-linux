'use strict';

// Phase-3 runtime smoke test for the Skyline `dart:ui` compatibility layer.
//
// The page exercises Application, WindowStatus, FramePhase.vsyncTarget,
// createNativeParagraph, warmUpFont and the SceneBuilder/PlatformDispatcher
// additions, then renders a solid green frame.  A green first pixel proves the
// engine was rebuilt from the patched platform SDK (the kernel alone cannot
// prove that the compat classes exist in the core snapshot).
//
// Skips when the engine, host, patched SDK or compiled smoke app is missing.
// Set SKYLINE_REQUIRE_ENGINE=1 to fail instead of skipping in CI.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const repoRoot = path.resolve(__dirname, '..', '..');
const host = path.join(repoRoot, 'native', 'build', 'skyline_host');
const kernel = path.join(
  repoRoot, 'apps', 'dart-ui-compat-smoke', 'build', 'flutter_assets', 'kernel_blob.bin',
);
const engineDir = path.join(repoRoot, 'native', 'engine');
const engineIcu = path.join(engineDir, 'out', 'linux-x64-debug', 'icudtl.dat');
const toolchainIcu = path.join(engineDir, 'toolchain', 'engine-artifacts', 'icudtl.dat');
const icu = fs.existsSync(engineIcu) ? engineIcu : toolchainIcu;
const engineLib = path.join(engineDir, 'out', 'linux-x64-debug', 'libflutter_engine.so');
const patchedSdk = path.join(engineDir, 'out', 'linux-x64-debug', 'patched_sdk', 'platform_strong.dill');

const width = 64;
const height = 48;
const expectedFrameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(host)) missing.push('native/build/skyline_host (npm run native:build)');
  if (!fs.existsSync(engineLib)) missing.push('libflutter_engine.so (npm run engine:build)');
  if (!fs.existsSync(patchedSdk)) missing.push('patched SDK (bash native/engine/patch-dart-ui.sh)');
  if (!fs.existsSync(kernel)) {
    missing.push('dart-ui-compat-smoke kernel (SKYLINE_APP_DIR=apps/dart-ui-compat-smoke npm run app:build -- --patched-sdk)');
  }
  if (!fs.existsSync(icu)) missing.push('icudtl.dat');
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

test('dart:ui compat: engine runs Application and renders a green frame', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, () => {
  const key = `ui_compat_${process.pid}_${Date.now().toString(36)}`;
  const shmPath = `/dev/shm/skyline_${key}.dat`;
  try {
    const result = spawnSync(host, [
      '--assets', path.dirname(kernel),
      '--icu', icu,
      '--width', String(width),
      '--height', String(height),
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
      return;
    }

    assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${diagnostics}`);
    assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${diagnostics}`);
    assert.equal(details.width, width);
    assert.equal(details.height, height);
    assert.equal(details.frameSize, expectedFrameSize);

    // stdout is flattened by the engine log callback; assert the Application
    // values survived the call.
    assert.match(result.stderr, /compat-smoke id=0 default=true stored=42 created=yes/);
    assert.match(result.stderr, /refreshRate=60\.0 parallel=false status=WindowStatus\.normal/);
    assert.match(result.stderr, /vsyncTarget=11/);
    assert.match(result.stderr, /windowStatus=WindowStatus\.normal/);

    const buffer = fs.readFileSync(shmPath);
    const dataSize = Number(buffer.readBigUInt64LE(0));
    assert.equal(buffer.length, 16 + dataSize);
    const data = buffer.subarray(16);
    const frameSize = data.readUInt32LE(4);
    assert.equal(frameSize, expectedFrameSize);
    const pixels = data.subarray(8, 8 + frameSize);
    assert.deepEqual([...pixels.subarray(0, 4)], [0, 255, 0, 255], 'first pixel must be opaque green');
    const center = (Math.floor(height / 2) * width + Math.floor(width / 2)) * 4;
    assert.deepEqual([...pixels.subarray(center, center + 4)], [0, 255, 0, 255], 'center pixel must be opaque green');
  } finally {
    try {
      fs.unlinkSync(shmPath);
    } catch {
      // Missing file is fine.
    }
  }
});

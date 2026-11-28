'use strict';

// Shared harness for tests and tools that launch native/build*/skyline_host:
// artifact paths, prerequisite checks, host invocation, shared-memory frames
// and pixel helpers.  Every host test goes through runHost() so the argument
// conventions, the frame protocol and the failure messages live in one place.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const { buildProblems } = require('../../tools/build-status');

const repoRoot = path.resolve(__dirname, '..', '..');
const engineRoot = path.join(repoRoot, 'native', 'engine');
const skylineBuild = path.join(engineRoot, 'work', 'skyline-app', 'build');
const toolchainIcu = path.join(engineRoot, 'toolchain', 'engine-artifacts', 'icudtl.dat');

const paths = {
  repoRoot,
  host: mode => path.join(repoRoot, 'native', mode === 'release' ? 'build-release' : 'build', 'skyline_host'),
  engineDir: mode => path.join(engineRoot, 'out', `linux-x64-${mode}`),
  engineLib: mode => path.join(paths.engineDir(mode), 'libflutter_engine.so'),
  icu(mode) {
    const staged = path.join(paths.engineDir(mode), 'icudtl.dat');
    return fs.existsSync(staged) || !fs.existsSync(toolchainIcu) ? staged : toolchainIcu;
  },
  // Standalone libskyline.so (Dart API DL only), built by CMake.
  stubRuntime: path.join(repoRoot, 'native', 'build', 'libskyline.so'),
  addon: mode => path.join(repoRoot, 'native', mode === 'release' ? 'build-release' : 'build', 'skyline.node'),
  skylineAssets: path.join(skylineBuild, 'flutter_assets'),
  skylineKernel: path.join(skylineBuild, 'flutter_assets', 'kernel_blob.bin'),
  skylineAot: path.join(skylineBuild, 'libapp.so'),
  appAssets: app => path.join(repoRoot, 'apps', app, 'build', 'flutter_assets'),
  appKernel: app => path.join(repoRoot, 'apps', app, 'build', 'flutter_assets', 'kernel_blob.bin'),
  appAot: app => path.join(repoRoot, 'apps', app, 'build', 'libapp.so'),
  fixture: name => path.join(repoRoot, 'test', 'fixtures',
    name.endsWith('.ops.json') ? name : `${name}.ops.json`),
};

// name -> [file, command that produces it]
const PREREQUISITES = {
  'host-debug': () => [paths.host('debug'), 'npm run native:build'],
  'host-release': () => [paths.host('release'), 'npm run native:build:release'],
  'engine-debug': () => [paths.engineLib('debug'), 'npm run engine:build'],
  'engine-release': () => [paths.engineLib('release'), 'npm run engine:build -- --runtime-mode release'],
  'icu-debug': () => [paths.icu('debug'), 'npm run engine:build'],
  'icu-release': () => [paths.icu('release'), 'npm run engine:build -- --runtime-mode release'],
  'skyline-kernel': () => [paths.skylineKernel, 'npm run kernel:skyline'],
  'skyline-aot': () => [paths.skylineAot, 'npm run devtools:aot'],
  'stub-runtime': () => [paths.stubRuntime, 'npm run native:build'],
  'addon-debug': () => [paths.addon('debug'), 'npm run native:build'],
  'empty-page-kernel': () => [paths.appKernel('empty-page'), 'npm run app:build'],
  'empty-page-aot': () => [paths.appAot('empty-page'), 'npm run app:aot'],
  'ui-compat-kernel': () => [paths.appKernel('dart-ui-compat-smoke'), 'npm run smoke:build'],
  'patched-sdk': () => [
    path.join(paths.engineDir('debug'), 'patched_sdk', 'platform_strong.dill'), 'npm run ui:patch'],
};

// The embedded Skyline runtime on the debug (JIT) and release (AOT) engines.
const SKYLINE_DEBUG = ['host-debug', 'engine-debug', 'icu-debug', 'skyline-kernel'];
const SKYLINE_RELEASE = ['host-release', 'engine-release', 'icu-release', 'skyline-aot'];

const reportedMissing = new Set();

// Missing prerequisites as `path (command)` strings; empty when all exist.
function missingPrerequisites(names) {
  const missing = [];
  for (const name of names) {
    const entry = PREREQUISITES[name];
    if (!entry) throw new Error(`unknown prerequisite: ${name}`);
    const [file, command] = entry();
    const shown = file.startsWith(repoRoot + path.sep) ? path.relative(repoRoot, file) : file;
    if (!fs.existsSync(file)) missing.push(`${shown} (${command})`);
  }
  return missing;
}

/**
 * node:test `skip` value for tests that need the named prerequisites: false
 * when all exist, otherwise the reason.  With SKYLINE_REQUIRE_ENGINE=1 a
 * missing prerequisite also registers one failing test per test file, so a run
 * that is required to exercise the engine cannot pass by skipping.
 */
function prerequisiteSkip(names) {
  const missing = missingPrerequisites(names);
  if (missing.length === 0) return false;
  const reason = `missing ${missing.join('; ')}`;
  if (process.env.SKYLINE_REQUIRE_ENGINE === '1' && !reportedMissing.has(reason)) {
    reportedMissing.add(reason);
    require('node:test').test('prerequisites required by SKYLINE_REQUIRE_ENGINE=1', () => {
      assert.fail(reason);
    });
  }
  return reason;
}

/**
 * `--entrypoint-arg` pairs for devtools.dart `main(args)`:
 * window id, runtime holder pointer, width, height, createRuntime and
 * skylineAddonPath.
 *
 * `skylineLibrary` is passed as skylineAddonPath.  package:skyline opens it by
 * path on Linux, exactly as the DevTools opens skyline.node, so it must be a
 * library that exports the Skyline ABI: the engine with the embedded runtime,
 * or the standalone stub `native/build/libskyline.so`.
 */
function devtoolsEntrypointArgs({
  width,
  height,
  skylineLibrary,
  windowId = 1,
  runtimeHolder = 1,
  createRuntime = true,
}) {
  if (!skylineLibrary) {
    throw new Error('devtoolsEntrypointArgs: skylineLibrary is required');
  }
  return [windowId, runtimeHolder, width, height, createRuntime ? 1 : 0, skylineLibrary]
    .flatMap(value => ['--entrypoint-arg', String(value)]);
}

/**
 * Fails the test `t` when the given build steps (tools/build-status.js, e.g.
 * `['engine-debug', 'kernel']`) are stale: host tests would otherwise exercise
 * an old embedded runtime or Dart snapshot and pass or fail for the wrong
 * reason.  `SKYLINE_ALLOW_STALE_BUILD=1` turns the failure into a skip.
 */
function checkBuildCurrent(t, steps) {
  const problems = buildProblems(steps);
  if (problems.length === 0) return;
  if (process.env.SKYLINE_ALLOW_STALE_BUILD === '1') {
    t.skip(`stale build allowed by SKYLINE_ALLOW_STALE_BUILD: ${problems.join('; ')}`);
    return;
  }
  throw new Error(
    'build artifacts are stale (set SKYLINE_ALLOW_STALE_BUILD=1 to run anyway):\n  ' +
    problems.join('\n  '));
}

// The JSON line skyline_host prints on exit (frame/vsync/present counters).
function parseResult(stdout) {
  const match = /^SKYLINE_HOST_RESULT (.*)$/m.exec(stdout || '');
  if (!match) return null;
  try {
    return JSON.parse(match[1]);
  } catch {
    return null;
  }
}

/**
 * Reads /dev/shm/skyline_<key>.dat (docs/contract.md section 4): a 16-byte
 * outer header (data size, version, padding) followed by the data region whose
 * first 8 bytes are the frame header (frameId, frameSize) and then RGBA pixels.
 */
function readFrame(file) {
  const buffer = fs.readFileSync(file);
  const dataSize = Number(buffer.readBigUInt64LE(0));
  const data = buffer.subarray(16);
  const frameSize = data.readUInt32LE(4);
  return {
    buffer,
    dataSize,
    version: buffer.readInt32LE(8),
    padding: buffer.readUInt32LE(12),
    data,
    frameId: data.readUInt32LE(0),
    frameSize,
    pixels: data.subarray(8, 8 + frameSize),
  };
}

function frameBytes(width, height, dpr = 1) {
  return Math.ceil(width * dpr) * Math.ceil(height * dpr) * 4;
}

function hostInvocation(options) {
  const mode = options.mode || 'debug';
  const width = options.width ?? 320;
  const height = options.height ?? 240;
  const key = `${options.key || 'skyline_test'}_${process.pid}_${Date.now().toString(36)}_` +
    Math.random().toString(36).slice(2, 8);
  const env = {
    // Tests assert on info-level runtime and host messages; set
    // SKYLINE_LOG_LEVEL=debug (or SKYLINE_APP_SERVICE_DEBUG=1) for traces.
    SKYLINE_LOG_LEVEL: 'info',
    ...process.env,
    ...(options.env || {}),
    LD_LIBRARY_PATH: [options.libraryPath || paths.engineDir(mode), process.env.LD_LIBRARY_PATH]
      .filter(Boolean).join(':'),
  };
  if (options.ops) env.SKYLINE_APP_SERVICE_OPS = `@${options.ops}`;
  const args = [
    '--assets', options.assets || paths.skylineAssets,
    '--icu', options.icu || paths.icu(mode),
    ...(options.aotLibrary ? ['--aot-library', options.aotLibrary] : []),
    '--width', String(width),
    '--height', String(height),
    ...(options.dpr === undefined ? [] : ['--dpr', String(options.dpr)]),
    '--shm-key', key,
    '--timeout-ms', String(options.timeoutMs ?? 20000),
    ...(options.frames === undefined ? [] : ['--frames', String(options.frames)]),
    ...(options.args || []),
    ...(options.noShutdown === false ? [] : ['--no-shutdown']),
    ...(options.entrypoint === false ? [] : devtoolsEntrypointArgs({
      width,
      height,
      skylineLibrary: options.skylineLibrary || paths.engineLib(mode),
    })),
  ];
  return {
    mode,
    host: paths.host(mode),
    args,
    env,
    shmPath: `/dev/shm/skyline_${key}.dat`,
    frameSize: options.frameSize ?? frameBytes(width, height, options.dpr ?? 1),
  };
}

function finishHost(invocation, result) {
  const logs = [
    result.stdout && result.stdout.trim(),
    result.stderr && result.stderr.trim(),
  ].filter(Boolean).join('\n');
  let frame = null;
  try {
    if (fs.existsSync(invocation.shmPath)) frame = readFrame(invocation.shmPath);
  } finally {
    fs.rmSync(invocation.shmPath, { force: true });
  }
  // Debug engines log every UI-thread-only runtime function that ran on
  // another thread (SKY_DCHECK_UI_THREAD); such a race must not pass a test.
  const threadViolation = /UI-thread check failed: .*/.exec(logs);
  if (threadViolation) {
    throw new Error(`${threadViolation[0]}\n${logs}`);
  }
  if (result.status === 77) {
    // The CMake stub host: the engine was missing when native/build* was
    // configured, although the prerequisite check found it now.
    throw new Error(`${invocation.host} is the exit-77 stub; rebuild it with ` +
      `npm run native:build${invocation.mode === 'release' ? ':release' : ''}\n${logs}`);
  }
  if (frame) {
    assert.equal(frame.buffer.length, 16 + frame.dataSize,
      'shared-memory file length must match the outer header');
    if (invocation.frameSize !== false) {
      assert.equal(frame.frameSize, invocation.frameSize, 'unexpected frame size');
    }
  }
  return {
    result,
    logs,
    details: parseResult(result.stdout),
    frame,
    pixels: frame ? frame.pixels : null,
  };
}

/**
 * Runs skyline_host once and returns
 * `{ result, logs, details, frame, pixels }` (`frame`/`pixels` are null when
 * no frame was written).  Options:
 *
 *   mode            'debug' (JIT kernel) or 'release' (AOT); default debug
 *   assets, icu     default: the recompiled Skyline app, the engine's ICU
 *   aotLibrary      passed as --aot-library
 *   width, height   CSS size (default 320x240); dpr adds --dpr
 *   ops             op fixture path (SKYLINE_APP_SERVICE_OPS=@ops)
 *   env             extra environment variables
 *   timeoutMs       host --timeout-ms (default 20000); frames adds --frames
 *   args            extra host arguments
 *   entrypoint      false for plain dart:ui apps (no devtools.dart arguments)
 *   skylineLibrary  devtools.dart skylineAddonPath (default: the engine)
 *   libraryPath     LD_LIBRARY_PATH entry (default: the engine directory)
 *   noShutdown      pass --no-shutdown (default true)
 *   frameSize       expected frame bytes (default from width/height/dpr);
 *                   false disables the check
 */
function runHost(options = {}) {
  const invocation = hostInvocation(options);
  const result = spawnSync(invocation.host, invocation.args, {
    encoding: 'utf8',
    timeout: options.spawnTimeoutMs ?? 45000,
    env: invocation.env,
  });
  return finishHost(invocation, result);
}

// runHost() without blocking the event loop, so independent host runs can
// overlap (node:test `concurrency`).
function runHostAsync(options = {}) {
  const invocation = hostInvocation(options);
  return new Promise((resolve, reject) => {
    const child = spawn(invocation.host, invocation.args, { env: invocation.env });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk; });
    child.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    const timer = setTimeout(() => child.kill('SIGKILL'), options.spawnTimeoutMs ?? 45000);
    child.on('error', error => {
      clearTimeout(timer);
      reject(error);
    });
    child.on('close', (status, signal) => {
      clearTimeout(timer);
      try {
        resolve(finishHost(invocation, { status, signal, stdout, stderr }));
      } catch (error) {
        reject(error);
      }
    });
  });
}

// RGBA at (x, y) of a frame `width` pixels wide.
function pixelAt(pixels, width, x, y) {
  const offset = (y * width + x) * 4;
  return [...pixels.subarray(offset, offset + 4)];
}

function countPixels(pixels, predicate) {
  let count = 0;
  for (let i = 0; i < pixels.length; i += 4) {
    if (predicate(pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3])) count++;
  }
  return count;
}

module.exports = {
  PREREQUISITES,
  SKYLINE_DEBUG,
  SKYLINE_RELEASE,
  checkBuildCurrent,
  countPixels,
  devtoolsEntrypointArgs,
  frameBytes,
  missingPrerequisites,
  parseResult,
  paths,
  pixelAt,
  prerequisiteSkip,
  readFrame,
  runHost,
  runHostAsync,
};

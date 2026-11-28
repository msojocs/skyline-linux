'use strict';

// skyline.node lifecycle: every threadsafe function the addon creates is
// released, so a process that loads the addon exits on its own, and the
// environment cleanup hook tears down live windows and sinks without a crash
// or hang.  Each scenario runs in its own Node process; they run concurrently
// because every process lives at least as long as the bootstrap's 20 s
// worklet-injection timer.

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { paths, prerequisiteSkip } = require('../../test/lib/host');

const skip = prerequisiteSkip(['addon-debug', 'engine-debug', 'empty-page-kernel']);
const bundle = path.dirname(paths.appAssets('empty-page'));

// Runs `body` (the source of an async function receiving `shell`, `frame`
// and `createWindow`) in a fresh Node process.
function runScenario(body, env = {}, timeoutMs = 45000) {
  const script = `
    'use strict';
    const addonPath = process.env.LIFECYCLE_ADDON;
    const bundle = process.env.LIFECYCLE_BUNDLE;
    require(addonPath);
    const shell = new global.SkylineShell();
    const width = 64;
    const height = 48;
    const frame = Buffer.alloc(8 + width * height * 4 * 4);
    const createWindow = (id) => shell.createWindow(
      id, bundle, width, height, 1, true, frame, addonPath, new Uint8Array(16));
    const nextFrame = (id) => new Promise((resolve) => {
      shell.onWindowRasterized(id, () => resolve(frame.readUInt32LE(0)));
    });
    (async (shell, frame, createWindow, nextFrame) => { ${body} })(
      shell, frame, createWindow, nextFrame).catch((error) => {
      console.error('scenario failed:', error && error.stack || error);
      process.exit(3);
    });
  `;
  return new Promise((resolve) => {
    const child = spawn(process.execPath, ['-e', script], {
      env: {
        SKYLINE_LOG_LEVEL: 'info',
        ...process.env,
        LIFECYCLE_ADDON: paths.addon('debug'),
        LIFECYCLE_BUNDLE: bundle,
        ...env,
      },
    });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk; });
    child.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    let error;
    const timer = setTimeout(() => {
      error = new Error(`timed out after ${timeoutMs} ms`);
      child.kill('SIGKILL');
    }, timeoutMs);
    child.on('close', (status, signal) => {
      clearTimeout(timer);
      resolve({ status, signal: error ? null : signal, error, stdout, stderr });
    });
  });
}

function assertCleanExit(result) {
  const output = `${result.stdout}\n${result.stderr}`;
  assert.equal(result.error, undefined, `process did not finish: ${result.error}\n${output}`);
  assert.equal(result.signal, null, `process was killed by ${result.signal}\n${output}`);
  assert.equal(result.status, 0, output);
}

describe('addon lifecycle', { skip, concurrency: true }, () => {
  it('an idle addon lets the process exit', { timeout: 60000 }, async () => {
    // The bootstrap installs the scroll/widget/JSON/HTTP sinks at load time.
    const result = await runScenario('');
    assertCleanExit(result);
    assert.match(result.stderr, /\[skyline-addon\] init ok/);
  });

  it('lifecycle messages go to SKYLINE_ADDON_LOG, stderr stays at warning', {
    timeout: 60000,
  }, async () => {
    const logFile = path.join(os.tmpdir(), `skyline-addon-lifecycle-${process.pid}.log`);
    fs.rmSync(logFile, { force: true });
    try {
      // An empty SKYLINE_LOG_LEVEL falls back to the default (warning).
      const result = await runScenario('', { SKYLINE_LOG_LEVEL: '', SKYLINE_ADDON_LOG: logFile });
      assertCleanExit(result);
      assert.doesNotMatch(result.stderr, /\[skyline-addon\]/);
      assert.match(fs.readFileSync(logFile, 'utf8'),
        /^\[pid \d+\] \[skyline-addon\] init begin\n(.*\n)*\[pid \d+\] \[skyline-addon\] init ok$/m);
    } finally {
      fs.rmSync(logFile, { force: true });
    }
  });

  it('destroying the window releases its frame callback', {
    timeout: 60000,
  }, async () => {
    const result = await runScenario(`
      createWindow(1);
      const frameId = await nextFrame(1);
      if (!(frameId >= 1)) throw new Error('no frame: ' + frameId);
      shell.destroyWindow(1);
      // Nothing else holds the event loop: the process must exit by itself.
    `);
    assertCleanExit(result);
  });

  it('a replaced frame callback is no longer called', {
    timeout: 60000,
  }, async () => {
    const result = await runScenario(`
      createWindow(2);
      await nextFrame(2);
      // Registering after the first frame replays that frame's notification to
      // the new callback; the callback it replaced must not hear about it.
      let first = 0;
      let second = 0;
      shell.onWindowRasterized(2, () => { first++; });
      await new Promise((resolve) => setTimeout(resolve, 200));
      shell.onWindowRasterized(2, () => { second++; });
      await new Promise((resolve) => setTimeout(resolve, 500));
      if (first !== 1 || second !== 1) {
        throw new Error('notifications: first=' + first + ' second=' + second);
      }
      shell.destroyWindow(2);
    `);
    assertCleanExit(result);
  });

  // process.exit() on the main thread skips environment teardown, so tear down
  // a worker's environment instead: the cleanup hook must join the engine and
  // abort the sinks while a window (and its loop-holding frame sink) is alive.
  it('environment teardown with a live window', {
    timeout: 60000,
  }, async () => {
    const result = await runScenario(`
      const { Worker } = require('node:worker_threads');
      const worker = new Worker(\`
        const addonPath = process.env.LIFECYCLE_ADDON;
        require(addonPath);
        const shell = new global.SkylineShell();
        const frame = Buffer.alloc(8 + 64 * 48 * 4 * 4);
        shell.createWindow(3, process.env.LIFECYCLE_BUNDLE, 64, 48, 1, true, frame,
          addonPath, new Uint8Array(16));
        shell.onWindowRasterized(3, () => process.exit(0));
      \`, { eval: true });
      const code = await new Promise((resolve) => worker.on('exit', resolve));
      if (code !== 0) throw new Error('worker exited with ' + code);
    `);
    assertCleanExit(result);
    assert.match(result.stderr, /window 3 stopped/);
  });
});

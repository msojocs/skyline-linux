'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { resultsDir, toWindowsPath, wineBinary } = require('./env');
const { ensureShim } = require('./win-shim');

const driverPath = path.resolve(__dirname, '..', 'driver.js');
const oracleAppDir = path.resolve(__dirname, '..', '..', 'tools', 'win-oracle-app');
const oracleShimDir = path.resolve(__dirname, '..', '..', 'tmp', 'win-oracle');

function safeName(id) {
  return id.replace(/[^A-Za-z0-9_.-]+/g, '_');
}

function tailFile(file, maxLines = 25) {
  try {
    const text = fs.readFileSync(file, 'utf8');
    return text.split(/\r?\n/).filter(Boolean).slice(-maxLines);
  } catch {
    return [];
  }
}

function buildInvocation(options, resultPath, logPath) {
  const { platform, caseId, paths, allowEngine } = options;
  if (platform === 'win32') {
    // Engine-context cases must run in a real Electron renderer: the official
    // module aborts in NodePlatform::ForIsolate when ELECTRON_RUN_AS_NODE
    // starts a Flutter engine.  tools/win-oracle-app reuses the same renderer
    // host as the L5 collector and writes the driver's result JSON.
    if (options.allowEngineContext) {
      const env = {
        ...process.env,
        WINEDEBUG: '-all',
        ORACLE_CASE: caseId,
        ORACLE_OUT: toWindowsPath(resultPath),
        ORACLE_DRIVER: toWindowsPath(driverPath),
        ORACLE_ALLOW_ENGINE: allowEngine ? '1' : '0',
        ORACLE_ALLOW_ENGINE_CONTEXT: '1',
      };
      if (!caseId.startsWith('create.') || caseId === 'create.destroyUnknownAfterAttempt') {
        env.ORACLE_CONTEXT_WINDOW = '1';
      }
      const optional = {
        ORACLE_MODULE: paths.addon,
        ORACLE_SHARED: paths.shared,
        ORACLE_RESOURCES: paths.resources,
        ORACLE_BUNDLE: paths.bundle,
        ORACLE_ADDON_PATH: paths.addon,
      };
      for (const [key, value] of Object.entries(optional)) {
        if (value) env[key] = toWindowsPath(value);
      }
      // The DevTools Electron is a packaged app; replace resources/app with
      // the oracle host instead of passing an app path on the command line.
      const shimElectron = ensureShim(paths.electron, oracleAppDir, oracleShimDir);
      return {
        command: wineBinary(),
        args: [shimElectron, '--no-sandbox'],
        env,
        logPath,
      };
    }
    const args = [
      toWindowsPath(driverPath),
      '--case', caseId,
      '--out', toWindowsPath(resultPath),
      '--platform', 'win32',
      '--module', toWindowsPath(paths.addon),
      '--shared', toWindowsPath(paths.shared),
      '--resources', toWindowsPath(paths.resources),
      '--bundle', toWindowsPath(paths.bundle),
      '--addon-path', toWindowsPath(paths.addon),
    ];
    if (allowEngine) args.push('--allow-engine');
    if (options.allowEngineContext) args.push('--allow-engine-context');
    return {
      command: wineBinary(),
      args: [paths.electron, ...args],
      env: {
        ...process.env,
        WINEDEBUG: '-all',
        ELECTRON_RUN_AS_NODE: '1',
      },
      logPath,
    };
  }
  const args = [
    driverPath,
    '--case', caseId,
    '--out', resultPath,
    '--platform', 'linux',
    '--shared', paths.shared || '',
  ];
  if (paths.addon) args.push('--module', paths.addon);
  if (paths.resources) args.push('--resources', paths.resources);
  if (paths.bundle) args.push('--bundle', paths.bundle);
  if (paths.addon) args.push('--addon-path', paths.addon);
  if (allowEngine) args.push('--allow-engine');
  if (options.allowEngineContext) args.push('--allow-engine-context');
  return {
    command: process.execPath,
    args,
    env: { ...process.env },
    logPath,
  };
}

function runOnce(options) {
  const { caseId, platform, timeoutMs = 30000 } = options;
  const directory = resultsDir();
  const base = path.join(directory, `${platform}-${safeName(caseId)}`);
  const resultPath = `${base}.json`;
  const logPath = `${base}.log`;
  try { fs.unlinkSync(resultPath); } catch {}
  try { fs.unlinkSync(logPath); } catch {}

  const invocation = buildInvocation(options, resultPath, logPath);
  const logFd = fs.openSync(logPath, 'w');
  let stderr = '';
  return new Promise(resolve => {
    const child = spawn(invocation.command, invocation.args, {
      env: invocation.env,
      stdio: ['ignore', logFd, logFd],
      windowsHide: true,
    });
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      try { child.kill('SIGKILL'); } catch {}
    }, timeoutMs);
    child.on('error', error => {
      clearTimeout(timer);
      fs.closeSync(logFd);
      resolve({ id: caseId, platform, status: 'spawn-error', error: { name: error.name, message: error.message } });
    });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      fs.closeSync(logFd);
      let payload = null;
      try {
        payload = JSON.parse(fs.readFileSync(resultPath, 'utf8'));
      } catch {}
      if (payload) {
        if (payload.status === 'crash' || payload.status === 'timeout') {
          const fileTail = tailFile(logPath);
          if (Array.isArray(payload.logTail) && payload.logTail.length) {
            payload.consoleTail = payload.logTail;
          }
          if (fileTail.length) payload.logTail = fileTail;
          else if (!Array.isArray(payload.logTail)) payload.logTail = [];
        }
        resolve(payload);
        return;
      }
      resolve({
        id: caseId,
        platform,
        status: timedOut ? 'timeout' : 'crash',
        exitCode: code,
        signal,
        logTail: tailFile(logPath),
      });
    });
  });
}

async function runCases(options) {
  const results = {};
  for (const caseId of options.caseIds) {
    results[caseId] = await runOnce({ ...options, caseId });
  }
  return results;
}

module.exports = { runOnce, runCases, safeName };

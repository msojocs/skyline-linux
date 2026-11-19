'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { resultsDir, toWindowsPath, wineBinary } = require('./env');

const driverPath = path.resolve(__dirname, '..', 'driver.js');

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

'use strict';

// Wine renderer host for recording engine-context contract cases from the
// official Windows skyline.node.
//
// `test/lib/runner.js` starts this app with the case id and module paths in the
// environment.  The renderer loads the official addon through
// `test/driver.js#executeCase` and sends the observation back over IPC.  The
// main process writes the same JSON result file the Node-mode driver writes,
// so `test/lib/runner.js` does not need a second result path.
//
// Why a renderer instead of ELECTRON_RUN_AS_NODE: the official module asserts
// inside NodePlatform::ForIsolate when createWindow starts its engine.  A real
// Electron renderer with nodeIntegration provides the host the module expects,
// which is the same trick tools/l5-win-app uses for L5 frames.

const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const outPath = process.env.ORACLE_OUT;
const caseId = process.env.ORACLE_CASE || null;
const events = [];
let finished = false;

function tail() {
  return events.slice(-25);
}

function finish(payload) {
  if (finished) return;
  finished = true;
  const result = {
    id: caseId,
    platform: 'win32',
    ...payload,
  };
  if (result.status !== 'ok' && !result.logTail) result.logTail = tail();
  try {
    if (outPath) fs.writeFileSync(outPath, JSON.stringify(result));
  } catch (error) {
    console.error('oracle result write failed', error);
  }
  app.exit(0);
  setTimeout(() => process.exit(0), 100);
}

ipcMain.on('oracle-result', (_event, payload) => {
  finish(payload || { status: 'error', error: { name: 'Error', message: 'empty oracle result' } });
});

app.commandLine.appendSwitch('no-sandbox');
app.commandLine.appendSwitch('disable-gpu-sandbox');
app.commandLine.appendSwitch('enable-logging');

app.whenReady().then(() => {
  const win = new BrowserWindow({
    width: 640,
    height: 480,
    show: false,
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
      sandbox: false,
      enableBlinkFeatures: 'SharedArrayBuffer',
      backgroundThrottling: false,
    },
  });
  win.webContents.on('console-message', (event, level, message) => {
    // Electron >= 36 passes a single event object; older builds use the
    // (event, level, message) tuple.
    const entry = event && event.message !== undefined
      ? { level: event.level, message: event.message }
      : { level, message };
    events.push({ kind: 'console', ...entry, message: String(entry.message).slice(0, 2000) });
  });
  win.webContents.on('render-process-gone', (_event, details) => {
    finish({ status: 'crash', exitCode: details && details.exitCode !== undefined ? details.exitCode : null });
  });
  win.webContents.on('did-fail-load', (_event, code, description) => {
    finish({ status: 'error', error: { name: 'load-failed', message: `${code} ${description}` } });
  });
  win.loadFile(path.join(__dirname, 'renderer.html'));
});

setTimeout(() => finish({ status: 'timeout' }), 60000);

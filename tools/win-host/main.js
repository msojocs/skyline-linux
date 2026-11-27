'use strict';

// Electron app that runs the official Windows skyline.node under Wine.
//
// The module asserts inside NodePlatform::ForIsolate when createWindow starts
// its engine under ELECTRON_RUN_AS_NODE; a real renderer with nodeIntegration
// provides the host it expects.  The official Electron is a packaged app and
// ignores a CLI app path, so test/lib/win-shim.js links this directory as its
// `resources/app`.
//
// SKYLINE_WIN_SCENARIO selects what the renderer does and which result file
// the caller expects:
//   l5      capture one L5 fixture frame (tools/l5-windows.js; L5_OUT gets
//           `{ ok, events, report }`, exit 0/1, 2 on a crash, 3 on timeout)
//   oracle  record one engine-context contract case (test/lib/runner.js;
//           ORACLE_OUT gets the same JSON the Node-mode driver writes)

const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const events = [];
let finished = false;

const SCENARIOS = {
  l5: {
    outPath: process.env.L5_OUT || path.join(__dirname, 'capture.json'),
    complete(payload) {
      const ok = !!(payload && payload.ok);
      return { body: JSON.stringify({ ok, events, report: payload }, null, 1), exitCode: ok ? 0 : 1 };
    },
    fail(kind, details) {
      events.push({ kind, details });
      return { body: JSON.stringify({ ok: false, events }, null, 1), exitCode: kind === 'timeout' ? 3 : 2 };
    },
  },
  oracle: {
    outPath: process.env.ORACLE_OUT,
    complete(payload) {
      const result = {
        id: process.env.ORACLE_CASE || null,
        platform: 'win32',
        ...(payload || { status: 'error', error: { name: 'Error', message: 'empty oracle result' } }),
      };
      if (result.status !== 'ok' && !result.logTail) result.logTail = events.slice(-25);
      return { body: JSON.stringify(result), exitCode: 0 };
    },
    fail(kind, details) {
      if (kind === 'timeout') return this.complete({ status: 'timeout' });
      if (kind === 'crash') {
        return this.complete({ status: 'crash', exitCode: details && details.exitCode !== undefined ? details.exitCode : null });
      }
      return this.complete({ status: 'error', error: { name: 'load-failed', message: details.message } });
    },
  },
};

const scenario = SCENARIOS[process.env.SKYLINE_WIN_SCENARIO];
if (!scenario) {
  console.error(`SKYLINE_WIN_SCENARIO must be one of ${Object.keys(SCENARIOS).join(', ')}`);
  process.exit(4);
}

function finish({ body, exitCode }) {
  if (finished) return;
  finished = true;
  try {
    if (scenario.outPath) fs.writeFileSync(scenario.outPath, body);
  } catch (error) {
    console.error('win-host result write failed', error);
  }
  app.exit(exitCode);
  setTimeout(() => process.exit(exitCode), 100);
}

ipcMain.on('win-host-log', (_event, entry) => { events.push(entry); });
ipcMain.on('win-host-result', (_event, payload) => { finish(scenario.complete(payload)); });

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
    events.push({ kind: 'console', level: entry.level, message: String(entry.message).slice(0, 4000) });
  });
  win.webContents.on('render-process-gone', (_event, details) => {
    finish(scenario.fail('crash', details));
  });
  win.webContents.on('did-fail-load', (_event, code, description) => {
    finish(scenario.fail('load-failed', { message: `${code} ${description}` }));
  });
  win.loadFile(path.join(__dirname, 'renderer.html'));
});

setTimeout(() => finish(scenario.fail('timeout', {})), 60000);

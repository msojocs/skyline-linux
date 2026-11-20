'use strict';
const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');

const outPath = process.env.L5_OUT || path.join(__dirname, 'capture.json');
const result = { ok: false, events: [] };
function writeResult(code) {
  try { fs.writeFileSync(outPath, JSON.stringify(result, null, 1)); } catch (error) { console.error('write', error); }
  const exitCode = typeof code === 'number' ? code : 0;
  app.exit(exitCode);
  setTimeout(() => process.exit(exitCode), 100);
}
ipcMain.on('l5-log', (_event, entry) => { result.events.push(entry); });
ipcMain.on('l5-finish', (_event, payload) => { result.report = payload; result.ok = !!(payload && payload.ok); writeResult(result.ok ? 0 : 1); });

app.commandLine.appendSwitch('no-sandbox');
app.commandLine.appendSwitch('disable-gpu-sandbox');
app.commandLine.appendSwitch('enable-logging');

app.whenReady().then(() => {
  const win = new BrowserWindow({
    width: 640, height: 480, show: false,
    webPreferences: {
      nodeIntegration: true, contextIsolation: false, sandbox: false,
      enableBlinkFeatures: 'SharedArrayBuffer', backgroundThrottling: false,
    },
  });
  win.webContents.on('console-message', (_event, level, message) => {
    result.events.push({ kind: 'console', level, message: String(message).slice(0, 4000) });
  });
  win.webContents.on('render-process-gone', (_event, details) => {
    result.events.push({ kind: 'render-process-gone', details });
    writeResult(2);
  });
  win.loadFile(path.join(__dirname, 'renderer.html'));
});
setTimeout(() => { result.events.push({ kind: 'timeout' }); writeResult(3); }, 60000);

'use strict';

const { ipcRenderer } = require('electron');
const fs = require('node:fs');
const path = require('node:path');

const driver = require(process.env.ORACLE_DRIVER);
const options = {
  caseId: process.env.ORACLE_CASE,
  platform: 'win32',
  modulePath: process.env.ORACLE_MODULE,
  sharedPath: process.env.ORACLE_SHARED,
  resources: process.env.ORACLE_RESOURCES || null,
  bundle: process.env.ORACLE_BUNDLE || null,
  addonPath: process.env.ORACLE_ADDON_PATH || null,
  allowEngine: process.env.ORACLE_ALLOW_ENGINE === '1',
  allowEngineContext: process.env.ORACLE_ALLOW_ENGINE_CONTEXT === '1',
};

// Some contract cases (brightness/destroy/notify) call into the engine without
// creating a window.  In the official module those calls dereference engine
// state and crash the process unless an engine context exists.  Create a
// disposable window first, using the same recipe as tools/l5-win-app.  Window
// 424242 cannot collide with the ids used by the cases (1/99/917).
async function createContextWindow() {
  if (process.env.ORACLE_CONTEXT_WINDOW !== '1') return null;
  // The official loader requires sharedMemory first and the addon second; the
  // addon installs global.SkylineShell/SkylineGlobal.
  require(process.env.ORACLE_SHARED);
  require(process.env.ORACLE_MODULE);
  const shared = require(process.env.ORACLE_SHARED);
  const shell = new global.SkylineShell();
  // Match tools/l5-win-app's callback registration: the engine calls resource
  // and teardown callbacks during initialization, so missing handlers can
  // turn into a null-call crash inside the renderer.
  for (const name of [
    'setNotifyBootstrapDoneCallback', 'setNotifyWindowReadyCallback',
    'setNotifyRouteDoneCallback', 'setNavigateBackCallback',
    'setNavigateBackDoneCallback', 'setSendLogCallback',
  ]) {
    try {
      shell[name]((...args) => console.log('oracle ctx', name, JSON.stringify(args).slice(0, 300)));
    } catch (_error) {}
  }
  shell.setLoadResourceCallback((request) => {
    const file = typeof request === 'string' ? request : (request && (request.path || request.name));
    try { return fs.readFileSync(file); } catch (_error) { return Buffer.alloc(0); }
  });
  shell.setLoadResourceAsyncCallback((request) =>
    console.log('oracle ctx loadResourceAsync', JSON.stringify(request).slice(0, 300)));
  const bundle = process.env.ORACLE_BUNDLE;
  shell.setHttpRequestCallback((request) => {
    console.log('oracle ctx httpRequest', JSON.stringify(request).slice(0, 300));
    const url = String(request && (request.url || request.path) || '');
    if (url.startsWith('file://')) {
      try { return fs.readFileSync(url.slice(7)); } catch (_error) {}
    }
    try { return fs.readFileSync(path.join(bundle, url.replace(/^[a-z]+:\/\/[^/]+/i, ''))); } catch (_error) {}
  });
  try { shell.setSafeAreaEdgeInsets(0, 0, 0, 0); } catch (_error) {}
  const ready = new Promise((resolve) => {
    try { shell.setNotifyWindowReadyCallback(() => resolve()); } catch (_error) {}
    setTimeout(resolve, 8000);
  });
  const key = `win_oracle_ctx_${process.pid}_${Date.now().toString(36)}`;
  const descriptor = shared.setMemory(key, 320 * 2 * 240 * 2 * 4 * 4 + 8);
  try {
    shell.createWindow(424242, process.env.ORACLE_BUNDLE, 320, 240, 2, true, null,
      process.env.ORACLE_MODULE, descriptor);
  } catch (error) {
    console.error('context window create failed', String(error && error.message || error));
  }
  await ready;
  // The L5 collector also triggers the app route and lets the engine settle
  // before touching SkylineGlobal/PageContext.
  try { shell.notifyAppLaunch(424242, 0, { backgroundColorContent: '#FFFFFFFF' }); } catch (_error) {}
  await new Promise((resolve) => setTimeout(resolve, 500));
  console.log('oracle: context window ready');
  return shell;
}

(async () => {
  const shell = await createContextWindow();
  if (shell) await new Promise((resolve) => setTimeout(resolve, 300));
  console.log('oracle: running case', options.caseId);
  try {
    const payload = await driver.executeCase({ ...options, shell });
    console.log('oracle: case result', payload.status);
    ipcRenderer.send('oracle-result', payload);
  } catch (error) {
    ipcRenderer.send('oracle-result', {
      status: 'error',
      error: {
        name: error && error.name || 'Error',
        message: String(error && error.message !== undefined ? error.message : error),
        stack: error && error.stack,
      },
    });
  }
})();

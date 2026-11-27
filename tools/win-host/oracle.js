'use strict';

// Oracle scenario: records one engine-context contract case from the official
// Windows skyline.node (test/lib/runner.js) through test/driver.js#executeCase.
const { ipcRenderer } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { openOfficialWindow } = require(path.join(__dirname, 'official-window.js'));

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
// state and crash the process unless an engine context exists, so create a
// disposable window first.  Window 424242 cannot collide with the ids used by
// the cases (1/99/917).
async function createContextWindow() {
  if (process.env.ORACLE_CONTEXT_WINDOW !== '1') return null;
  const bundle = process.env.ORACLE_BUNDLE;
  const log = (...args) => console.log('oracle ctx', ...args);
  let windowReady;
  const ready = new Promise((resolve) => {
    windowReady = resolve;
    setTimeout(resolve, 8000);
  });
  let shell;
  try {
    ({ shell } = openOfficialWindow({
      sharedPath: process.env.ORACLE_SHARED,
      addonPath: process.env.ORACLE_MODULE,
      bundle,
      windowId: 424242,
      width: 320,
      height: 240,
      dpr: 2,
      key: `win_oracle_ctx_${process.pid}_${Date.now().toString(36)}`,
      log,
      onWindowReady: () => windowReady(),
      safeAreaInsets: true,
      httpRequest: (request) => {
        log('httpRequest', JSON.stringify(request).slice(0, 300));
        const url = String(request && (request.url || request.path) || '');
        if (url.startsWith('file://')) {
          try { return fs.readFileSync(url.slice(7)); } catch (_error) {}
        }
        try { return fs.readFileSync(path.join(bundle, url.replace(/^[a-z]+:\/\/[^/]+/i, ''))); } catch (_error) {}
      },
    }));
  } catch (error) {
    console.error('context window create failed', String(error && error.message || error));
    shell = shell || new global.SkylineShell();
  }
  await ready;
  // Like the L5 scenario, trigger the app route and let the engine settle
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
    ipcRenderer.send('win-host-result', payload);
  } catch (error) {
    ipcRenderer.send('win-host-result', {
      status: 'error',
      error: {
        name: error && error.name || 'Error',
        message: String(error && error.message !== undefined ? error.message : error),
        stack: error && error.stack,
      },
    });
  }
})();

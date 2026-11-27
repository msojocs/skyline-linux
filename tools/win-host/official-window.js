'use strict';

// Opens a window on the official Windows skyline.node inside the Electron
// renderer: loads sharedMemory.node and then skyline.node (the DevTools loader
// order; the addon installs global.SkylineShell), registers the callbacks the
// engine calls during initialization (a missing handler can become a null call
// inside the renderer), and creates the window on a shared-memory frame buffer.

const fs = require('fs');

const LIFECYCLE_CALLBACKS = [
  'setNotifyBootstrapDoneCallback', 'setNotifyWindowReadyCallback',
  'setNotifyRouteDoneCallback', 'setNavigateBackCallback',
  'setNavigateBackDoneCallback', 'setSendLogCallback',
];

/**
 * Options: sharedPath, addonPath, bundle, windowId, width, height, dpr, key,
 * log(...args), onWindowReady(), httpRequest(request) (default: log only),
 * safeAreaInsets (call setSafeAreaEdgeInsets(0, 0, 0, 0) first).
 * Returns { shell, shared, descriptor, created }.
 */
function openOfficialWindow(options) {
  const log = options.log;
  const shared = require(options.sharedPath);
  require(options.addonPath);
  const shell = new global.SkylineShell();
  for (const name of LIFECYCLE_CALLBACKS) {
    try {
      shell[name]((...args) => {
        log(name, JSON.stringify(args).slice(0, 300));
        if (name === 'setNotifyWindowReadyCallback' && options.onWindowReady) options.onWindowReady();
      });
    } catch (error) {
      log(name, 'register error', String(error && error.message || error));
    }
  }
  shell.setLoadResourceCallback((request) => {
    const file = typeof request === 'string' ? request : (request && (request.path || request.name));
    log('loadResource', String(file));
    try { return fs.readFileSync(file); } catch (_error) { return Buffer.alloc(0); }
  });
  shell.setLoadResourceAsyncCallback((request) =>
    log('loadResourceAsync', JSON.stringify(request).slice(0, 300)));
  shell.setHttpRequestCallback(options.httpRequest ||
    ((request) => log('httpRequest', JSON.stringify(request).slice(0, 300))));
  if (options.safeAreaInsets) {
    try { shell.setSafeAreaEdgeInsets(0, 0, 0, 0); } catch (_error) {}
  }
  const frameSize = options.width * options.dpr * options.height * options.dpr * 4;
  const descriptor = shared.setMemory(options.key, frameSize * 4 + 8);
  const created = shell.createWindow(options.windowId, options.bundle, options.width,
    options.height, options.dpr, true, null, options.addonPath, descriptor);
  return { shell, shared, descriptor, created };
}

module.exports = { openOfficialWindow };

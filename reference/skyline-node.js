'use strict';

// Reference implementation of the JS-visible contract of the official
// `skyline.node` module.  It contains no rendering engine: `createWindow`
// reports that the engine backend is not attached.  Its purpose is to serve as
// an executable specification and as the API layer that a native (or
// Wine-backed) backend can be plugged into.
//
// Contract source: spec/skyline-node.win32.json (recorded from the official
// Windows module under Wine).  Run:
//   SKYLINE_LINUX_SKYLINE_ADDON=reference/skyline-node.js npm run test:contract

const DEBUG_INFO = {
  skyline_git_rev: 'ef3edf6c94265dfe314f9197ff4e100f99aac070',
  flutter_engine_git_rev: '6b917cf86ebe1b9ff148310504a736d667527e8b',
  skyline_version: '1.4.21',
};

const CREATE_WINDOW_ARITY_MESSAGE = 'shell.run expects 8 arguments: window_id bundle_path, width, height, device_pixel_ratio, hide_window, array_buffer, skyline_addon_path[, electron_array_buffer]';

function createShellClass() {
  const windows = new Map();
  const callbacks = new Map();

  function SkylineShell(...args) {
    if (!new.target) return undefined;
    // The native object has no own properties.
  }

  const prototype = SkylineShell.prototype;

  function defineMethod(name, implementation) {
    Object.defineProperty(prototype, name, {
      value: implementation,
      enumerable: true,
      writable: true,
      configurable: false,
    });
  }

  function setter(method) {
    return function (...args) {
      if (args.length !== 1 || typeof args[0] !== 'function') {
        throw new Error(`${'Set' + method.slice(3)} expects a function argument`);
      }
      callbacks.set(method, args[0]);
    };
  }

  function dispatch(name, arity) {
    return function (...args) {
      if (args.length !== arity) throw new Error(`${name} expects ${arity} arguments`);
      return undefined;
    };
  }

  // Definition order is part of the contract: the official prototype returns
  // this exact order from Object.getOwnPropertyNames(), with `constructor`
  // last (the native implementation defines it after the methods).
  const definitions = [
    ['setNotifyBootstrapDoneCallback', setter('setNotifyBootstrapDoneCallback')],
    ['createWindow', function (...args) {
      if (args.length < 8 || (args.length < 9 && !args[8])) throw new Error(CREATE_WINDOW_ARITY_MESSAGE);
      const handle = args[8];
      if (!ArrayBuffer.isView(handle) || handle.byteLength !== 16) {
        throw new Error('invalid shared memory handle');
      }
      throw new Error('Skyline engine backend is not attached (reference implementation)');
    }],
    ['destroyWindow', function (...args) {
      if (args.length > 0 && windows.has(args[0])) windows.delete(args[0]);
    }],
    ['onWindowRasterized', function (...args) {
      if (args.length !== 2) {
        throw new Error('shell.OnWindowRasterized expects 2 arguments: windowId, callback');
      }
      const [windowId, callback] = args;
      if (!windows.has(windowId)) throw new Error(`window_id: ${windowId} not found`);
      windows.get(windowId).rasterized = callback;
    }],
    ['setNotifyWindowReadyCallback', setter('setNotifyWindowReadyCallback')],
    ['setNotifyRouteDoneCallback', setter('setNotifyRouteDoneCallback')],
    ['setNavigateBackCallback', setter('setNavigateBackCallback')],
    ['setNavigateBackDoneCallback', setter('setNavigateBackDoneCallback')],
    ['setLoadResourceCallback', setter('setLoadResourceCallback')],
    ['setLoadResourceAsyncCallback', setter('setLoadResourceAsyncCallback')],
    ['setHttpRequestCallback', setter('setHttpRequestCallback')],
    ['setSendLogCallback', setter('setSendLogCallback')],
    ['setSafeAreaEdgeInsets', function (...args) {
      if (args.length !== 4) throw new Error('SetSafeAreaEdgeInsets expects 4 arguments');
      for (const value of args) Number(value);
    }],
    ['notifyResourceLoad', function () { return undefined; }],
    ['notifyHttpRequestComplete', function () { return undefined; }],
    ['dispatchTouchStartEvent', dispatch('dispatchTouchStartEvent', 3)],
    ['dispatchTouchMoveEvent', dispatch('dispatchTouchMoveEvent', 3)],
    ['dispatchTouchEndEvent', dispatch('dispatchTouchEndEvent', 3)],
    ['dispatchTouchOverEvent', dispatch('dispatchTouchOverEvent', 3)],
    ['dispatchTouchCancelEvent', dispatch('dispatchTouchCancelEvent', 1)],
    ['dispatchWheelEvent', dispatch('dispatchWheelEvent', 5)],
    ['dispatchCharEvent', dispatch('dispatchCharEvent', 2)],
    ['dispatchKeyboardEvent', dispatch('dispatchKeyboardEvent', 5)],
    ['notifyNavigateTo', function () { return undefined; }],
    ['notifyRedirectTo', function () { return undefined; }],
    ['notifyNavigateBack', function () { return undefined; }],
    ['notifyAppLaunch', function () { return undefined; }],
    ['notifyReLaunch', function () { return undefined; }],
    ['notifyAutoReLaunch', function () { return undefined; }],
    ['notifySwitchTab', function () { return undefined; }],
    ['onPlatformBrightnessChanged', function () { return undefined; }],
  ];

  for (const [name, implementation] of definitions) defineMethod(name, implementation);

  delete prototype.constructor;
  Object.defineProperty(prototype, 'constructor', {
    value: SkylineShell,
    enumerable: false,
    writable: true,
    configurable: true,
  });
  Object.defineProperty(SkylineShell, 'name', { value: '' });
  Object.defineProperty(SkylineShell, 'length', { value: 0 });
  // Native (non-strict) function objects also have own `arguments`/`caller`
  // properties; strict-mode function declarations do not.
  Object.defineProperty(SkylineShell, 'arguments', { value: null, writable: false, enumerable: false, configurable: false });
  Object.defineProperty(SkylineShell, 'caller', { value: null, writable: false, enumerable: false, configurable: false });

  return { SkylineShell };
}

const { SkylineShell } = createShellClass();

const SkylineDebugInfo = { ...DEBUG_INFO };

global.SkylineShell = SkylineShell;
global.SkylineDebugInfo = SkylineDebugInfo;

module.exports = {};

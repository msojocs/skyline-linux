'use strict';

// Embedded bootstrap: builds the JS-visible surface of the official module and
// forwards engine operations to the native addon object (globalThis.__skylineNative).
// It intentionally contains no `require`/`module` references because it runs via
// napi_run_script inside the addon's context.

(() => {
  const native = globalThis.__skylineNative;
  if (!native) throw new Error('native Skyline bridge is missing');

  const CREATE_WINDOW_ARITY_MESSAGE = 'shell.run expects 8 arguments: window_id bundle_path, width, height, device_pixel_ratio, hide_window, array_buffer, skyline_addon_path[, electron_array_buffer]';

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
      return native.setCallback(method, args[0]);
    };
  }

  function dispatch(name, arity) {
    return function (...args) {
      if (args.length !== arity) throw new Error(`${name} expects ${arity} arguments`);
      return native.dispatch(name, args);
    };
  }

  const definitions = [
    ['setNotifyBootstrapDoneCallback', setter('setNotifyBootstrapDoneCallback')],
    ['createWindow', function (...args) {
      if (args.length < 8 || (args.length < 9 && !args[8])) throw new Error(CREATE_WINDOW_ARITY_MESSAGE);
      const handle = args[8];
      if (!ArrayBuffer.isView(handle) || handle.byteLength !== 16) {
        throw new Error('invalid shared memory handle');
      }
      return native.createWindow(args);
    }],
    ['destroyWindow', function (...args) {
      return native.destroyWindow(args.length > 0 ? args[0] : undefined);
    }],
    ['onWindowRasterized', function (...args) {
      if (args.length !== 2) {
        throw new Error('shell.OnWindowRasterized expects 2 arguments: windowId, callback');
      }
      return native.onWindowRasterized(args[0], args[1]);
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
      return native.setSafeArea(args[0], args[1], args[2], args[3]);
    }],
    ['notifyResourceLoad', function (...args) { return native.notify('notifyResourceLoad', args); }],
    ['notifyHttpRequestComplete', function (...args) { return native.notify('notifyHttpRequestComplete', args); }],
    ['dispatchTouchStartEvent', dispatch('dispatchTouchStartEvent', 3)],
    ['dispatchTouchMoveEvent', dispatch('dispatchTouchMoveEvent', 3)],
    ['dispatchTouchEndEvent', dispatch('dispatchTouchEndEvent', 3)],
    ['dispatchTouchOverEvent', dispatch('dispatchTouchOverEvent', 3)],
    ['dispatchTouchCancelEvent', dispatch('dispatchTouchCancelEvent', 1)],
    ['dispatchWheelEvent', dispatch('dispatchWheelEvent', 5)],
    ['dispatchCharEvent', dispatch('dispatchCharEvent', 2)],
    ['dispatchKeyboardEvent', dispatch('dispatchKeyboardEvent', 5)],
    ['notifyNavigateTo', function (...args) { return native.notify('notifyNavigateTo', args); }],
    ['notifyRedirectTo', function (...args) { return native.notify('notifyRedirectTo', args); }],
    ['notifyNavigateBack', function (...args) { return native.notify('notifyNavigateBack', args); }],
    ['notifyAppLaunch', function (...args) { return native.notify('notifyAppLaunch', args); }],
    ['notifyReLaunch', function (...args) { return native.notify('notifyReLaunch', args); }],
    ['notifyAutoReLaunch', function (...args) { return native.notify('notifyAutoReLaunch', args); }],
    ['notifySwitchTab', function (...args) { return native.notify('notifySwitchTab', args); }],
    ['onPlatformBrightnessChanged', function (...args) { return native.brightness(args.length > 0 ? args[0] : undefined); }],
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
  Object.defineProperty(SkylineShell, 'arguments', { value: null, writable: false, enumerable: false, configurable: false });
  Object.defineProperty(SkylineShell, 'caller', { value: null, writable: false, enumerable: false, configurable: false });

  global.SkylineShell = SkylineShell;
  global.SkylineDebugInfo = native.debugInfo();
})();

    ['setNotifyBootstrapDoneCallback', setter('setNotifyBootstrapDoneCallback')],
    ['createWindow', function (...args) {
      if (args.length < 8 || (args.length < 9 && !args[8])) throw new Error(CREATE_WINDOW_ARITY_MESSAGE);
      const handle = args[8];
      // Windows passes a 16-byte pointer/size descriptor (typed array); the
      // Linux DevTools passes the external ArrayBuffer from setMemory().
      if (handle !== undefined && handle !== null &&
          !ArrayBuffer.isView(handle) && !(handle instanceof ArrayBuffer)) {
        throw new Error('invalid shared memory handle');
      }
      installCreateWindowResponseGate();
      // The official devtools_shell.cc applies Number() to the numeric
      // arguments; the DevTools passes some of them as strings.
      const normalized = args.slice();
      for (const index of [0, 2, 3, 4]) {
        const value = normalized[index];
        if (typeof value === 'string' && value.trim() !== '') {
          const number = Number(value);
          if (Number.isFinite(number)) normalized[index] = number;
        }
      }
      const created = native.createWindow(normalized);
      const windowId = normalized[0];
      if (Number.isFinite(normalized[2]) && Number.isFinite(normalized[3]) &&
          normalized[2] > 0 && normalized[3] > 0) {
        windowSizes.set(windowId, { width: normalized[2], height: normalized[3] });
      }
      // Resolve the extension's createWindow promise immediately so it emits
      // requireRenderContext; the response gate holds CREATE_WINDOW_RESPONSE
      // until the app_sub_render frame is ready.
      setTimeout(() => {
        if (typeof shellCallbacks.setNotifyWindowReadyCallback === 'function') {
          shellCallbacks.setNotifyWindowReadyCallback(windowId);
        }
      }, 0);
      return created;
    }],    ['destroyWindow', function (...args) {
      // Match the official module: exactly one window id, and an unknown id is
      // an error rather than a silent no-op.  Number('x') is NaN, which the
      // official error message renders as 0.
      if (args.length !== 1) {
        throw new Error('shell.destroy expects 1 arguments: windowId, ');
      }
      const numeric = Number(args[0]);
      const windowId = Number.isFinite(numeric) ? numeric : 0;
      if (!windowSizes.has(windowId)) {
        throw new Error(`window_id: ${windowId} not found`);
      }
      windowSizes.delete(windowId);
      rasterizedCallbacks.delete(windowId);
      if (rasterizedCallbacks.size === 0) stopFramePump();
      return native.destroyWindow(windowId);
    }],
    ['onWindowRasterized', function (...args) {
      if (args.length !== 2) {
        throw new Error('shell.OnWindowRasterized expects 2 arguments: windowId, callback');
      }
      const windowId = args[0];
      const callback = args[1];
      if (typeof callback !== 'function') {
        return native.onWindowRasterized(windowId, callback);
      }
      const entry = { callback, lastPresent: Date.now() };
      rasterizedCallbacks.set(windowId, entry);
      // Native uses this wrapper for real presents; the pump calls `callback`
      // directly so its own messages do not keep the pump alive.
      const nativeCallback = function (event) {
        entry.lastPresent = Date.now();
        ensureFramePump();
        return callback(event);
      };
      try {
        const result = native.onWindowRasterized(windowId, nativeCallback);
        ensureFramePump();
        return result;
      } catch (error) {
        rasterizedCallbacks.delete(windowId);
        if (rasterizedCallbacks.size === 0) stopFramePump();
        throw error;
      }
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
    ['notifyHttpRequestComplete', function (...args) {
      const [requestId, errno, status, headers, buffer] = args;
      if (typeof native.log === 'function') {
        native.log(`notifyHttpRequestComplete ${requestId} ` +
          `len=${buffer && typeof buffer.length === 'number' ? buffer.length : 0}`);
      }
      if (typeof native.httpRequestComplete === 'function') {
        return native.httpRequestComplete(requestId,
          buffer || new Uint8Array(0));
      }
      return native.notify('notifyHttpRequestComplete',
        [requestId, errno, status, headers, buffer]);
    }],
    ['dispatchTouchStartEvent', dispatchTouch('dispatchTouchStartEvent', 'start', 3)],
    ['dispatchTouchMoveEvent', dispatchTouch('dispatchTouchMoveEvent', 'move', 3)],
    ['dispatchTouchEndEvent', dispatchTouch('dispatchTouchEndEvent', 'end', 3)],
    ['dispatchTouchOverEvent', dispatchTouch('dispatchTouchOverEvent', 'over', 3)],
    ['dispatchTouchCancelEvent', dispatchTouch('dispatchTouchCancelEvent', 'cancel', 1)],
    ['dispatchWheelEvent', function (...args) {
      if (args.length !== 5) throw new Error('dispatchWheelEvent expects 5 arguments');
      return native.dispatchWheel(args[0], args[1], args[2], args[3], args[4]);
    }],
    ['dispatchCharEvent', dispatch('dispatchCharEvent', 2)],
    ['dispatchKeyboardEvent', dispatch('dispatchKeyboardEvent', 5)],
    ['notifyNavigateTo', function (...args) {
      sendNotifyOp('notifyNavigateTo', args);
      if (typeof native.log === 'function') {
        native.log(`notifyNavigateTo window=${args[0]} page=${args[1]} task=${args[2]}`);
      }
      completeRoute(args[2]);
      return undefined;
    }],
    ['notifyRedirectTo', function (...args) {
      sendNotifyOp('notifyRedirectTo', args);
      if (typeof native.log === 'function') {
        native.log(`notifyRedirectTo window=${args[0]} page=${args[1]} task=${args[2]}`);
      }
      completeRoute(args[2]);
      return undefined;
    }],
    ['notifyNavigateBack', function (...args) {
      sendNotifyOp('notifyNavigateBack', args);
      // Mirror the native page-stack pop so the hit test uses the page that is
      // visible again after the back animation.
      try {
        const windowId = typeof args[0] === 'number' ? args[0] : 0;
        const delta = typeof args[2] === 'number' && args[2] > 0 ? args[2] : 1;
        const stack = attachedRootStack.get(windowId);
        if (stack && stack.length > 1) {
          for (let i = 0; i < delta && stack.length > 1; i += 1) stack.pop();
          attachedRoots.set(windowId, stack[stack.length - 1]);
        }
      } catch (_error) {}
      if (typeof native.log === 'function') {
        native.log(`notifyNavigateBack window=${args[0]} page=${args[1]} task=${args[4]}`);
      }
      completeRoute(args[4]);
      return undefined;
    }],
    ['notifyAppLaunch', function (...args) {
      sendNotifyOp('notifyAppLaunch', args);
      if (typeof native.log === 'function') {
        native.log(`notifyAppLaunch window=${args[0]} page=${args[1]}`);
      }
      // The first onAppRoute is dispatched before the SDK subscribes; make
      // sure the launch page exists (replaying the route only when the
      // native dispatch was dropped), then complete the DevTools handshake so
      // its routeDone finds the created page.  appLaunch routes use taskId 0.
      ensureLaunchPage(args[0], args[1]).then(() => completeRoute(0));
      // Fallback: if the launch route initialised the app but produced no
      // page, replay it once through the SDK after the route has settled.
      setTimeout(() => retryLaunchIfNoPage(0), 1500);
      return undefined;
    }],
    ['notifyReLaunch', function (...args) {
      sendNotifyOp('notifyReLaunch', args);
      if (typeof native.log === 'function') {
        native.log(`notifyReLaunch window=${args[0]} page=${args[1]}`);
      }
      completeRoute(0);
      return undefined;
    }],
    ['notifyAutoReLaunch', function (...args) {
      sendNotifyOp('notifyAutoReLaunch', args);
      completeRoute(0);
      return undefined;
    }],
    ['notifySwitchTab', function (...args) {
      sendNotifyOp('notifySwitchTab', args);
      if (typeof native.log === 'function') {
        native.log(`notifySwitchTab window=${args[0]} page=${args[1]}`);
      }
      completeRoute(0);
      return undefined;
    }],
    ['onPlatformBrightnessChanged', function (...args) {
      if (args.length !== 1) {
        throw new Error('OnPlatformBrightnessChanged expects 1 argument');
      }
      return native.brightness(args[0]);
    }],
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

  // ---------------------------------------------------------------------------
  // Live SkylineGlobal bridge
  //
  // The miniprogram's Skyline render context builds the page tree through
  // `SkylineGlobal.PageContext`.  We record the same element operations the
  // recorder captures and hand the finished page to the native runtime, which
  // applies them through the existing appservice page builder.
  // ---------------------------------------------------------------------------
  let nextBridgeTag = 0;
  const opsByWindow = new Map();
  const jsValues = new Map();
  let nextJsValueId = 0;
  // compileStyleSheet(path) -> id; appendCompiledStyleSheet(scope, id) replays
  // the raw WXSS through the native CSS parser when the resource loader can
  // return text.
  const compiledStyleSheets = new Map();
  let nextStyleSheetId = 0;
  // Logical window size per window id, captured from createWindow.  Selector
  // queries resolve the viewport rect from the backend context's
  // clientWidth/clientHeight.
  const windowSizes = new Map();
  // PageContexts whose initial render finished; incremental DOM mutations
  // (setData -> class/style/attribute updates) are flushed on the next tick.
  const liveContexts = new Set();

  function windowSize(windowId) {
    return windowSizes.get(windowId === undefined ? 0 : windowId) || null;
  }

  function zeroRect() {
    return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 };
  }

  // Returns the node's viewport rect, or null when the layout snapshot has not
  // been published yet (the page build runs on the engine UI thread, so an
  // `attached()` selector query can race it).
  function nodeRect(windowId, viewTag) {
    if (typeof native.getNodeRect !== 'function') return null;
    try {
      const rect = native.getNodeRect(windowId, viewTag);
      return rect && typeof rect.width === 'number' ? rect : null;
    } catch (_error) {
      return null;
    }
  }

  function bridgeOps(windowId) {
    const key = windowId === undefined || windowId === null ? 0 : windowId;
    let ops = opsByWindow.get(key);
    if (!ops) {
      ops = [];
      opsByWindow.set(key, ops);
    }
    return ops;
  }


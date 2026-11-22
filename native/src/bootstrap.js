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

  const shellCallbacks = Object.create(null);

  // The DevTools dispatches its first `onAppRoute` as soon as createWindow
  // resolves, but the appservice only creates the `app_sub_render` frame after
  // `requireRenderContext` and that frame needs ~1 s to install its SDK.  If the
  // route arrives before the frame exists it is dropped and the simulator stays
  // white.  Hold the `SKYLINE_CREATE_WINDOW_RESPONSE` message (the project waits
  // on it before dispatching the route) until the frame reports ready.
  const SKYLINE_CREATE_WINDOW_RESPONSE = 'SAC6';
  const RENDER_CONTEXT_TIMEOUT_MS = 8000;
  let heldWindowResponses = [];
  let renderContextPolling = false;

  function renderContextDetail() {
    const detail = {
      frameReady: false, engine: false, bridge: false, global: false,
      app: false, window: false, found: false,
    };
    try {
      const maps = typeof window !== 'undefined' && window.__global &&
        window.__global.contextFrameMaps;
      if (!maps || typeof maps.forEach !== 'function') return detail;
      maps.forEach((frame) => {
        if (!frame) return;
        try {
          const win = frame.iframe && frame.iframe.contentWindow;
          if (frame.name === 'app' && win) {
            // The generated app code must be evaluated before a route can
            // create a page in the logic layer.
            detail.app = typeof win.getApp === 'function' && !!win.getApp();
          }
          if (frame.name !== 'app_sub_render' || !win) return;
          detail.found = true;
          if (frame._ready === true) detail.frameReady = true;
          detail.engine = !!win.__skylineEngine__;
          detail.bridge = !!win.WeixinJSBridge;
          // SkylineGlobal is injected by the onSkylineGlobalReady replay,
          // which happens after the frame's engine exists.  Requiring it
          // ensures the route pipeline is subscribed before the route runs.
          detail.global = !!win.SkylineGlobal;
          if (win.__skylineEngine__) {
            try {
              const runtime = win.__skylineEngine__.getRuntime();
              const windows = runtime && runtime.allWindows;
              const count = windows && (windows.size !== undefined
                ? windows.size
                : Object.keys(windows).length);
              detail.window = count > 0;
            } catch (_error) {}
          }
        } catch (_error) {}
      });
    } catch (_error) {}
    return detail;
  }

  function renderContextState() {
    const detail = renderContextDetail();
    if (!detail.found) return 'missing';
    const ready = detail.frameReady && detail.engine && detail.bridge &&
      detail.global && detail.app && detail.window;
    return ready ? 'ready' : 'created';
  }

  function releaseHeldWindowResponses(state) {
    const held = heldWindowResponses;
    heldWindowResponses = [];
    for (const entry of held) {
      if (typeof native.log === 'function') {
        native.log(`release CREATE_WINDOW_RESPONSE window=${entry.windowId} ` +
          `renderContext=${state}`);
      }
      try {
        entry.send();
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`release window response failed: ${error && error.message}`);
        }
      }
    }
  }

  function waitForRenderContext(attempt, readyTicks) {
    const state = renderContextState();
    const waited = attempt * 50;
    // Keep the response held for ~500 ms after the frame first reports ready:
    // `app_sub_render` exposes its engine before the `onSkylineGlobalReady`
    // replay finishes installing the route pipeline.
    const ticks = state === 'ready' ? readyTicks + 1 : 0;
    if ((state === 'ready' && ticks >= 12) || waited >= RENDER_CONTEXT_TIMEOUT_MS) {
      if (typeof native.log === 'function') {
        const detail = renderContextDetail();
        native.log(`render context settle: state=${state} waited=${waited}ms ` +
          `frame=${detail.frameReady} engine=${detail.engine} ` +
          `bridge=${detail.bridge} global=${detail.global} ` +
          `app=${detail.app} window=${detail.window}`);
      }
      renderContextPolling = false;
      releaseHeldWindowResponses(state);
      return;
    }
    renderContextPolling = true;
    setTimeout(() => waitForRenderContext(attempt + 1, ticks), 50);
  }

  function installCreateWindowResponseGate() {
    try {
      const hasWindow = typeof window !== 'undefined';
      const pageGlobal = hasWindow ? (window.__global__ || window.__global) : null;
      const messager = pageGlobal && pageGlobal.__messager__;
      if (!messager || typeof messager.send !== 'function' || messager.__skylineGate) {
        return false;
      }
      const originalSend = messager.send;
      messager.send = function (...args) {
        const data = args[2];
        if (data && data.command === SKYLINE_CREATE_WINDOW_RESPONSE) {
          const windowId = data.data && data.data.windowId;
          heldWindowResponses.push({
            windowId,
            send: () => originalSend.apply(messager, args),
          });
          if (typeof native.log === 'function') {
            native.log(`hold CREATE_WINDOW_RESPONSE window=${windowId}`);
          }
          if (!renderContextPolling) waitForRenderContext(0);
          return undefined;
        }
        return originalSend.apply(messager, args);
      };
      messager.__skylineGate = true;
      return true;
    } catch (_error) {
      return false;
    }
  }

  function setter(method) {
    return function (...args) {
      if (args.length !== 1 || typeof args[0] !== 'function') {
        throw new Error(`${'Set' + method.slice(3)} expects a function argument`);
      }
      shellCallbacks[method] = args[0];
      native.setCallback(method, args[0]);
      if (method === 'setNotifyBootstrapDoneCallback') {
        // The engine is ready as soon as the addon loads.  Notify synchronously:
        // the DevTools sends the `appLaunch` route right after createWindow, and
        // its route handler is gated on this flag.  Deferring with setTimeout(0)
        // can drop the route and make the simulator time out after 10 s.
        shellCallbacks.setNotifyBootstrapDoneCallback();
      }
      return undefined;
    };
  }

  function dispatch(name, arity) {
    return function (...args) {
      if (args.length !== arity) throw new Error(`${name} expects ${arity} arguments`);
      return native.dispatch(name, args);
    };
  }

  // Touch events are forwarded to the Flutter engine as pointer events so the
  // simulator's scroll views and tap gestures work.  They are also dispatched
  // to the appservice's glass-easel tree (bindtap / catch:touchstart handlers)
  // through the touch callback registered by Skyline RuntimeCore.
  const TOUCH_EVENT_TYPES = {
    start: 'touchstart', move: 'touchmove', end: 'touchend', cancel: 'touchcancel',
  };
  function dispatchTouch(name, kind, arity) {
    return function (...args) {
      if (args.length !== arity) throw new Error(`${name} expects ${arity} arguments`);
      const windowId = args.length > 0 ? args[0] : 0;
      const x = args.length > 1 ? args[1] : 0;
      const y = args.length > 2 ? args[2] : 0;
      const result = native.dispatchTouch(kind, windowId, x, y);
      const domType = TOUCH_EVENT_TYPES[kind];
      if (domType) {
        try {
          dispatchDomTouchEvent(windowId, domType, x, y);
        } catch (error) {
          if (typeof native.log === 'function') {
            native.log(`dom touch dispatch failed: ${error && error.message}`);
          }
        }
      }
      return result;
    };
  }

  // The DevTools' RouteController matches SKYLINE_NOTIFY_ROUTE_DONE by the
  // `taskId` it put into the route request.  `appLaunch`/`reLaunch`/`switchTab`
  // use 0; navigateTo/redirectTo pass it as the third argument and navigateBack
  // as the fifth.  Passing the window id (the previous behaviour) never matched
  // and left every Skyline route to time out after 10 s.
  function completeRoute(taskId) {
    const value = typeof taskId === 'number' && Number.isFinite(taskId)
      ? taskId
      : 0;
    setTimeout(() => {
      if (typeof shellCallbacks.setNotifyRouteDoneCallback === 'function') {
        shellCallbacks.setNotifyRouteDoneCallback(value);
      }
    }, 500);
  }

  // The very first `onAppRoute` at launch initializes the app but, on this
  // Electron build, does not always create the page (the SDK defers page
  // creation until the app lifecycle has settled).  A re-route after the app
  // is up does create it, so retry once when the logic layer still has no
  // page.  The route is replayed through the SDK rather than faking ops.
  function appFrameWindow() {
    try {
      const maps = typeof window !== 'undefined' && window.__global &&
        window.__global.contextFrameMaps;
      let app = null;
      if (maps && typeof maps.forEach === 'function') {
        maps.forEach((frame) => {
          if (frame && frame.name === 'app' && frame.iframe) {
            try { app = frame.iframe.contentWindow; } catch (_error) {}
          }
        });
      }
      return app;
    } catch (_error) {
      return null;
    }
  }

  // The DevTools dispatches the initial `onAppRoute` while the appservice
  // SDK is still loading, so the route engines never see the `appLaunch`
  // route and the later routeDone reports `webviewId N is not found`.  Once
  // the Skyline window is up we replay the same route through the real
  // appservice bridge, which creates the page before routeDone is sent.
  function triggerBridgeEvent(win, eventName, data, webviewId) {
    try {
      const pageGlobal = win && (win.__global || win.__global__);
      const bridge = pageGlobal && pageGlobal.WeixinJSBridge;
      if (bridge && typeof bridge.__triggerOnEvent === 'function') {
        bridge.__triggerOnEvent(eventName, data, webviewId);
        return true;
      }
    } catch (_error) {}
    return false;
  }

  function replayAppLaunchRoute(windowId, pageId) {
    const pageGlobal = typeof window !== 'undefined'
      ? (window.__global || window.__global__)
      : null;
    if (!pageGlobal ||
        typeof pageGlobal.getInstanceWindow !== 'function') {
      return Promise.resolve(false);
    }
    let path = '';
    let query = {};
    let scene = 1001;
    try {
      const app = appFrameWindow();
      const options = app && app.wx && app.wx.getLaunchOptionsSync &&
        app.wx.getLaunchOptionsSync();
      if (options) {
        path = options.path || '';
        query = options.query || {};
        if (typeof options.scene === 'number') scene = options.scene;
      }
    } catch (_error) {}
    if (!path) return Promise.resolve(false);
    const data = {
      path,
      webviewId: pageId,
      openType: 'appLaunch',
      query,
      scene,
      renderer: 'skyline',
      singlePageData: {
        windowId,
        pageId,
        isNewEngineHomePage: windowId === pageId,
      },
    };
    // Only the main appservice engine (`instanceframe`) receives the route:
    // it fans it out to the app/render sub-contexts just like the DevTools
    // broadcast did.  Triggering the sub-contexts directly creates duplicate
    // pages (`appLaunch with an already exist webviewId`).
    return Promise.resolve(pageGlobal.getInstanceWindow()).then((win) => {
      // The original route may have been delivered while the instance window
      // promise settled; re-check before replaying to avoid duplicate pages.
      if (hasLaunchPage()) return false;
      const replayed = triggerBridgeEvent(win, 'onAppRoute', data, pageId);
      if (replayed && typeof native.log === 'function') {
        native.log(`replayed appLaunch route webview=${pageId} ` +
          'into the appservice engine');
      }
      return replayed;
    }).catch(() => false);
  }

  // The first onAppRoute is a race: sometimes the SDK drops it, sometimes it
  // is delivered just as `notifyAppLaunch` runs.  Wait briefly for the native
  // route to create the page, and only replay when it is really missing.
  function ensureLaunchPage(windowId, pageId) {
    return new Promise((resolve) => {
      const started = Date.now();
      const poll = () => {
        if (hasLaunchPage()) {
          if (typeof native.log === 'function') {
            native.log(`launch page ready after ${Date.now() - started}ms`);
          }
          resolve(false);
          return;
        }
        if (Date.now() - started >= 500) {
          replayAppLaunchRoute(windowId, pageId).then(() => resolve(true));
          return;
        }
        setTimeout(poll, 50);
      };
      poll();
    });
  }

  // A page engine that already created the launch page must not receive the
  // replayed route (the SDK reports "already exist webviewId").
  function hasLaunchPage() {
    for (const candidate of [appFrameWindow(), typeof window !== 'undefined'
      ? window : null]) {
      try {
        if (candidate && typeof candidate.getCurrentPages === 'function' &&
            candidate.getCurrentPages().length > 0) {
          return true;
        }
      } catch (_error) {}
    }
    return false;
  }

  function pageStackSummary() {
    let main = -1;
    try {
      main = typeof getCurrentPages === 'function' ? getCurrentPages().length : -1;
    } catch (_error) {}
    let app = -1;
    try {
      const frame = appFrameWindow();
      app = frame && typeof frame.getCurrentPages === 'function'
        ? frame.getCurrentPages().length
        : -1;
    } catch (_error) {}
    return `main=${main} app=${app}`;
  }

  function retryLaunchIfNoPage(attempt) {
    const app = appFrameWindow();
    if (!app) {
      if (attempt < 6) setTimeout(() => retryLaunchIfNoPage(attempt + 1), 500);
      return;
    }
    let pages = 0;
    try {
      pages = typeof app.getCurrentPages === 'function'
        ? app.getCurrentPages().length
        : 0;
    } catch (_error) {
      pages = 0;
    }
    if (pages > 0) {
      if (typeof native.log === 'function') {
        native.log(`initial route created ${pages} page(s) ` +
          `(${pageStackSummary()})`);
      }
      return;
    }
    if (attempt >= 2) return;
    let path = '';
    try {
      const options = app.wx && app.wx.getLaunchOptionsSync &&
        app.wx.getLaunchOptionsSync();
      path = options && options.path ? options.path : '';
    } catch (_error) {}
    if (!path) return;
    const url = '/' + path.replace(/^\/+/, '');
    try {
      if (app.wx && typeof app.wx.reLaunch === 'function') {
        if (typeof native.log === 'function') {
          native.log(`retry initial route via reLaunch ${url} ` +
            `(${pageStackSummary()})`);
        }
        app.wx.reLaunch({ url });
      }
    } catch (error) {
      if (typeof native.log === 'function') {
        native.log(`reLaunch retry failed: ${error && error.message}`);
      }
    }
    setTimeout(() => retryLaunchIfNoPage(attempt + 1), 1500);
  }

  // Route notifications (`notifyNavigateTo`/`notifyNavigateBack`/...) used to
  // be dropped because `native.notify` is a no-op in the addon.  Route them
  // through the live-ops channel so the runtime can pop/restore pages; the
  // subsequent row of `attach` ops materializes the new page route.
  function sendNotifyOp(kind, args) {
    const list = Array.prototype.slice.call(args || []);
    const windowId = typeof list[0] === 'number' ? list[0] : 0;
    let payload;
    try {
      payload = JSON.stringify({ ops: [{ op: kind, args: list }] });
    } catch (_error) {
      return;
    }
    try {
      if (typeof native.applyPageOps === 'function') {
        native.applyPageOps(windowId, payload);
      }
    } catch (_error) {}
  }

  // -- pageframe frame pump ---------------------------------------------------
  // The pageframe only pulls a new frame after receiving
  // SKYLINE_WINDOW_RASTERIZED.  That message crosses the appservice ->
  // pageframe process boundary, and its arrival occasionally slips past a
  // display animation-frame tick.  The pageframe then repeats the previous
  // frame even though the engine already presented the next one, which is
  // visible as a 33 ms stutter.  Replaying the registered rasterized callback
  // from a timer keeps the pageframe's copy flag set, so every RAF consumes
  // the newest shared-memory frame.  The pump idles out 250 ms after the last
  // native present, so a static page does not keep sending messages.  Only the
  // DevTools appservice has a pageframe to feed.
  const rasterizedCallbacks = new Map();
  let framePumpTimer = null;
  const FRAME_PUMP_INTERVAL_MS = 8;
  const FRAME_PUMP_IDLE_MS = 250;
  function framePumpEligible() {
    try {
      return typeof window !== 'undefined' && !!window.__global &&
        !!window.__global.contextFrameMaps;
    } catch (_error) {
      return false;
    }
  }
  function ensureFramePump() {
    if (framePumpTimer !== null || rasterizedCallbacks.size === 0 ||
        !framePumpEligible()) {
      return;
    }
    framePumpTimer = setInterval(() => {
      const now = Date.now();
      let active = false;
      for (const [windowId, entry] of rasterizedCallbacks) {
        if (now - entry.lastPresent > FRAME_PUMP_IDLE_MS) continue;
        active = true;
        try {
          entry.callback({ window_id: windowId });
        } catch (_error) {}
      }
      if (!active) stopFramePump();
    }, FRAME_PUMP_INTERVAL_MS);
  }
  function stopFramePump() {
    if (framePumpTimer !== null) {
      clearInterval(framePumpTimer);
      framePumpTimer = null;
    }
  }

  const definitions = [
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
    ['notifyHttpRequestComplete', function (...args) { return native.notify('notifyHttpRequestComplete', args); }],
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

  function bridgeId(value) {
    if (value === undefined || value === null) return 0;
    return typeof value === 'object' ? value.viewTag : value;
  }

  function bridgeScalar(value) {
    if (value === undefined) return undefined;
    const type = typeof value;
    if (type === 'string' || type === 'number' || type === 'boolean') {
      return value;
    }
    try {
      return JSON.parse(JSON.stringify(value));
    } catch (_error) {
      return String(value);
    }
  }

  function bridgePlain(value) {
    if (value === undefined || value === null) return null;
    try {
      return JSON.parse(JSON.stringify(value));
    } catch (_error) {
      return null;
    }
  }

  // glass-easel's spliceBefore(beforeNode, count, ...nodes) passes a ShadowNode
  // where the generic splice() takes an index.  Both forms are normalised to a
  // child index.  A missing/unknown anchor appends at the end.
  function childIndexIn(children, value) {
    if (typeof value === 'number' && Number.isFinite(value)) {
      const index = Math.trunc(value);
      return index < 0 || index > children.length ? children.length : index;
    }
    if (value === undefined || value === null) return children.length;
    const index = children.indexOf(value);
    return index < 0 ? children.length : index;
  }

  // Fragments are recorded by id; the native replay flattens their children.
  // The JS-side mirror of the tree needs the flattened form.
  function splitFragment(item) {
    if (item === undefined || item === null || typeof item !== 'object') return [];
    return item.tag === 'fragment' ? item.children.slice() : [item];
  }

  // Defensive serialisation: a single ShadowNode captured in an op payload used
  // to make JSON.stringify throw (its `ops` array points back at the operation),
  // which aborted the whole render.  Nodes are replaced by their id and other
  // cycles are dropped so one malformed op cannot blank the simulator.
  function serializeOps(ops) {
    const seen = new WeakSet();
    return JSON.stringify({ ops }, (key, value) => {
      if (value === null || typeof value !== 'object') return value;
      if (typeof value.viewTag === 'number' && Array.isArray(value.ops)) {
        return value.viewTag;
      }
      if (seen.has(value)) return null;
      seen.add(value);
      return value;
    });
  }

  // ShadowNode arguments (parent/children links) are cyclic, so logging them
  // with a plain JSON.stringify throws inside the miniprogram SDK.  Collapse
  // nodes to their viewTag and drop leftover cycles.
  function debugArgs(args) {
    const seen = new WeakSet();
    try {
      return JSON.stringify(args, (key, value) => {
        if (value === null || typeof value !== 'object') return value;
        if (typeof value.viewTag === 'number' && Array.isArray(value.ops)) {
          return `#${value.viewTag}`;
        }
        if (seen.has(value)) return '[circular]';
        seen.add(value);
        return value;
      });
    } catch (_error) {
      return '[unserializable]';
    }
  }

  function toText(data) {
    if (data === null || data === undefined) return null;
    if (typeof data === 'string') return data;
    try {
      let bytes = null;
      if (data instanceof ArrayBuffer) {
        bytes = new Uint8Array(data);
      } else if (ArrayBuffer.isView(data)) {
        bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      }
      if (!bytes) return null;
      return new TextDecoder('utf-8', { fatal: false }).decode(bytes);
    } catch (_error) {
      return null;
    }
  }

  function looksLikeCss(text) {
    if (typeof text !== 'string' || text.length === 0) return false;
    // Reject binary blobs: require mostly printable text and CSS punctuation.
    let printable = 0;
    for (let i = 0; i < text.length && i < 512; i += 1) {
      const code = text.charCodeAt(i);
      if (code === 9 || code === 10 || code === 13 || (code >= 32 && code < 127) ||
          code > 159) {
        printable += 1;
      }
    }
    if (printable < Math.min(text.length, 512) * 0.95) return false;
    return text.indexOf('{') >= 0 && (text.indexOf(':') >= 0 || text.indexOf(';') >= 0);
  }

  function describeData(data, text) {
    if (data === null || data === undefined) return 'null';
    const length = typeof data.byteLength === 'number'
      ? data.byteLength
      : (typeof data.length === 'number' ? data.length : -1);
    const head = typeof text === 'string' ? text.slice(0, 120).replace(/\s+/g, ' ') : '';
    return `len=${length} text=${looksLikeCss(text) ? 'css' : 'no'} head="${head}"`;
  }

  // The DevTools appservice exposes the compiled WXSS as base64 text at
  // `/appservice/__skyline__/<path>.wxss` (the base `wxlibfile://` library
  // stylesheets are not served there).  Use the appservice's own XHR so this
  // works in every frame of the appservice webview.
  function loadStyleSheetText(path) {
    if (typeof path !== 'string' || path.startsWith('wxlibfile://')) return null;
    try {
      const pageGlobal = (typeof window !== 'undefined' &&
        (window.__global || window.__global__)) || null;
      const Xhr = pageGlobal && pageGlobal.XMLHttpRequest;
      if (typeof Xhr !== 'function') return null;
      const xhr = new Xhr();
      xhr.open('GET', `/appservice/__skyline__/${path}.wxss`, false);
      xhr.send(null);
      if (xhr.status !== 200 || !xhr.response) return null;
      const atobFn = (pageGlobal && pageGlobal.atob) ||
        (typeof atob === 'function' ? atob : null);
      if (typeof atobFn !== 'function') return null;
      const binary = atobFn(xhr.response);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i += 1) {
        bytes[i] = binary.charCodeAt(i);
      }
      return new TextDecoder('utf-8', { fatal: false }).decode(bytes);
    } catch (_error) {
      return null;
    }
  }

  function autoStub(target) {
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        return () => ++nextBridgeTag;
      },
    });
  }

  // The miniprogram SDK builds worklets with `module.createWorklet(fn)()`,
  // expecting the wrapper to return a callable.  A plain autoStub returns a
  // number there, which breaks component creation before the page tree is
  // built.  Returning the input function keeps the JS-side worklet callable.
  // `wx.worklet.shared(initial)` goes through makeMutable and the component
  // reads/writes `.value` (address-book's property observer does this), so the
  // mutable stub has to behave like the JS-thread fallback: a value holder.
  function workletMutable(initial) {
    let current = initial;
    return {
      get value() { return current; },
      set value(next) { current = next; },
      addListener() {},
      removeListener() {},
    };
  }

  function workletStub() {
    const target = Object.create(null);
    const passthrough = function (value) {
      return typeof value === 'function' ? value : undefined;
    };
    target.installCoreFunctions = function (setter) { return setter; };
    target.makeShareable = function (value) { return value; };
    target.makeMutable = workletMutable;
    target.makeRemote = function (value) { return value; };
    target.startMapper = function () { return 0; };
    target.stopMapper = function () {};
    target.stopMappers = function () {};
    target.enableMapper = function () {};
    target.disableMapper = function () {};
    target.pushFrame = function () {};
    target.requestFrame = function () {};
    target.getCurrentTime = function () { return Date.now(); };
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        object[property] = passthrough;
        return passthrough;
      },
    });
  }

  // The miniprogram SDK's worklet code references the native-provided
  // `workletUIModule` as a bare global (`setGlobalConsole`,
  // `getBoundingClientRect`, `scrollTo`, ...).  The DevTools appservice never
  // installs it, so the first worklet run throws
  // `ReferenceError: workletUIModule is not defined` at launch.  Provide the
  // methods the SDK probes for; the widget helpers fall back to the SDK's own
  // zero-rect/no-op behaviour.
  function createWorkletUIModule() {
    return {
      setGlobalConsole() {},
      getBoundingClientRect() {
        return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 };
      },
      getCurrentTime() { return Date.now(); },
      navigatorPop() {},
      requestFrame() {},
      scrollTo() {},
      snapshotChangeMode() {},
      startUserGesture() {},
      stopUserGesture() {},
      updateNodeStyle() {},
    };
  }

  function installWorkletUIModule(win) {
    if (!win) return;
    try {
      if (!win.workletUIModule) win.workletUIModule = createWorkletUIModule();
    } catch (_error) {}
  }

  function installWorkletUIModules() {
    if (typeof window !== 'undefined') installWorkletUIModule(window);
    if (typeof globalThis !== 'undefined' &&
        (typeof window === 'undefined' || globalThis !== window)) {
      installWorkletUIModule(globalThis);
    }
    try {
      const pageGlobal = typeof window !== 'undefined'
        ? (window.__global || window.__global__)
        : null;
      const maps = pageGlobal && pageGlobal.contextFrameMaps;
      if (maps && typeof maps.forEach === 'function') {
        maps.forEach((frame) => {
          try {
            const win = frame && frame.iframe && frame.iframe.contentWindow;
            if (win) installWorkletUIModule(win);
          } catch (_error) {}
        });
      }
      if (pageGlobal && typeof pageGlobal.getInstanceWindow === 'function') {
        Promise.resolve(pageGlobal.getInstanceWindow()).then((win) => {
          installWorkletUIModule(win);
        }).catch(() => {});
      }
    } catch (_error) {}
  }

  class BridgeElement {
    constructor(tag, name, ops, windowId) {
      this.ops = ops || [];
      this.tag = tag;
      this.name = name;
      this.windowId = windowId;
      this.attrs = {};
      this.children = [];
      this.parent = null;
      this.text = '';
      this._id = '';
      this._style = '';
      this.viewTag = ++nextBridgeTag;
      this.ops.push({ op: 'createElement', id: this.viewTag, tag, name });
    }
    get tagName() { return this.tag; }
    get id() { return this._id; }
    set id(value) { this.setId(value); }
    get style() { return this._style; }
    set style(value) { this.setStyle(value); }
    get textContent() { return this.text; }
    set textContent(value) { this.setText(value); }
    get length() { return this.children.length; }
    setText(value) {
      this.text = String(value);
      this.ops.push({ op: 'setText', id: this.viewTag, text: this.text });
    }
    setId(value) {
      this._id = String(value);
      this.ops.push({ op: 'setId', id: this.viewTag, value: this._id });
    }
    setAttribute(name, value) {
      this.attrs[name] = value;
      this.ops.push({ op: 'setAttribute', id: this.viewTag, name, value: bridgeScalar(value) });
    }
    removeAttribute(name) {
      delete this.attrs[name];
      this.ops.push({ op: 'removeAttribute', id: this.viewTag, name });
    }
    setStyle(value) {
      this._style = String(value);
      this.ops.push({ op: 'setStyle', id: this.viewTag, value: this._style });
    }
    addClass(prefix, scope) {
      this.ops.push({ op: 'addClass', id: this.viewTag, prefix, name: scope });
    }
    removeClass(prefix, scope) {
      this.ops.push({ op: 'removeClass', id: this.viewTag, prefix, name: scope });
    }
    setClass(names) {
      this.ops.push({ op: 'setClass', id: this.viewTag, names: String(names) });
    }
    setStyleScope(scope) {
      this.ops.push({ op: 'setStyleScope', id: this.viewTag, scope: String(scope) });
    }
    associateComponent() {
      this.ops.push({ op: 'associateComponent', id: this.viewTag });
    }
    setListenerOption(name, option) {
      this.ops.push({ op: 'setListenerOption', id: this.viewTag, name, option: bridgeScalar(option) });
    }
    setAttributes(values) {
      this.ops.push({ op: 'setAttributes', id: this.viewTag, attrs: bridgePlain(values) });
    }
    setAttributeValue(name, value) {
      this.ops.push({ op: 'setAttributeValue', id: this.viewTag, name, value: bridgeScalar(value) });
    }
    setHitTestBehavior(value) {
      this.ops.push({ op: 'setHitTestBehavior', id: this.viewTag, value });
    }
    addAnimatedStyle(style) {
      this.ops.push({
        op: 'addAnimatedStyle',
        id: this.viewTag,
        mapperId: style && typeof style === 'object' ? style.mapperId : undefined,
      });
    }
    asShadowRoot() {
      this.ops.push({ op: 'asShadowRoot', id: this.viewTag });
      return this;
    }
    forceDetached() {}
    // Native backend mode passes a callback (Domlike mode reads the return
    // value directly).  The renderer answers queries only after pending
    // mutations have been laid out; our snapshot is published at the end of
    // the engine-side build, so wait for it instead of reporting a stale 0x0
    // rect (which breaks `attached()` measurements such as the A-Z index).
    getBoundingClientRect(callback) {
      const rect = nodeRect(this.windowId, this.viewTag);
      if (typeof callback !== 'function') return rect || zeroRect();
      if (rect) {
        setTimeout(() => callback(rect), 0);
        return undefined;
      }
      // A partially built tree can publish a 0x0 rect before the final
      // layout; wait for a non-empty box (a genuinely empty node falls back to
      // the latest rect at the deadline).
      const deadline = Date.now() + 1500;
      let last = rect;
      const poll = () => {
        const next = nodeRect(this.windowId, this.viewTag);
        if (next) {
          last = next;
          if (next.width > 0 || next.height > 0) {
            setTimeout(() => callback(next), 0);
            return;
          }
        }
        if (Date.now() >= deadline) {
          callback(last || zeroRect());
        } else {
          setTimeout(poll, 16);
        }
      };
      setTimeout(poll, 16);
      return undefined;
    }
    getScrollOffset(callback) {
      const rect = nodeRect(this.windowId, this.viewTag);
      const offset = {
        scrollLeft: 0,
        scrollTop: 0,
        scrollWidth: rect ? rect.width : 0,
        scrollHeight: rect ? rect.height : 0,
      };
      if (typeof callback === 'function') {
        setTimeout(() => callback(offset), 0);
        return undefined;
      }
      return offset;
    }
    getAttribute(name) { return this.attrs[name]; }
    cloneNode() {
      const clone = new BridgeElement(this.tag, this.name, this.ops, this.windowId);
      clone.attrs = Object.assign({}, this.attrs);
      clone.text = this.text;
      clone._style = this._style;
      this.ops.push({ op: 'cloneNode', id: this.viewTag, clone: clone.viewTag });
      return clone;
    }
    appendChild(child) {
      if (!child) return;
      this.ops.push({ op: 'appendChild', id: this.viewTag, child: bridgeId(child) });
      this.children.push(child);
      child.parent = this;
    }
    insertBefore(child, before) {
      if (!child) return;
      this.ops.push({
        op: 'insertBefore', id: this.viewTag, child: bridgeId(child),
        before: bridgeId(before),
      });
      const index = this.children.indexOf(before);
      this.children.splice(index < 0 ? this.children.length : index, 0, child);
      child.parent = this;
    }
    removeChild(child) {
      if (!child) return;
      this.ops.push({ op: 'removeChild', id: this.viewTag, child: bridgeId(child) });
      const index = this.children.indexOf(child);
      if (index >= 0) this.children.splice(index, 1);
      child.parent = null;
    }
    replaceChild(child, index) {
      // The glass-easel backend may pass either a numeric index or the node it
      // replaces; normalise before recording so the op stays JSON-safe.
      const position = childIndexIn(this.children, index);
      this.ops.push({ op: 'replaceChild', id: this.viewTag, child: bridgeId(child), index: position });
      if (position >= 0 && position < this.children.length) {
        this.children[position] = child;
      } else {
        this.children.push(child);
      }
      child.parent = this;
    }
    findChildPosition(child) { return this.children.indexOf(child); }
    splice(position, count, ...items) {
      // glass-easel uses both `splice(index, count, ...nodes)` and
      // `spliceBefore(beforeNode, count, fragment)`.  As seen in WASubContext,
      // the latter passes a real ShadowNode as `position`; recording that node
      // directly made `JSON.stringify` fail with a circular structure and
      // aborted every page render.  Collapse nodes to their child index here.
      const pos = childIndexIn(this.children, position);
      const deleteCount = typeof count === 'number' && Number.isFinite(count)
        ? Math.max(0, Math.trunc(count))
        : 0;
      const flatItems = [];
      for (const item of items) {
        if (Array.isArray(item)) {
          for (const nested of item) {
            if (nested !== undefined && nested !== null) flatItems.push(nested);
          }
        } else if (item !== undefined && item !== null) {
          flatItems.push(item);
        }
      }
      this.ops.push({
        op: 'splice', id: this.viewTag, pos,
        count: deleteCount, items: flatItems.map(bridgeId),
      });
      const flat = [];
      for (const item of flatItems) flat.push(...splitFragment(item));
      this.children.splice(pos, deleteCount, ...flat);
      for (const child of flat) child.parent = this;
    }
    // Remaining ShadowNode API surface used by the glass-easel renderer.  The
    // subset above produces the ops the native runtime replays; these keep the
    // JS page framework running without changing the rendered tree.
    getParentNode() { return this.parent; }
    getChildNode(index) { return this.children[index] || null; }
    get classList() { return []; }
    // Layout offsets read directly by builtin components (swiper/scroll views
    // use them to compute gesture deltas).
    get offsetTop() {
      const rect = nodeRect(this.windowId, this.viewTag);
      return rect ? rect.top : 0;
    }
    get offsetLeft() {
      const rect = nodeRect(this.windowId, this.viewTag);
      return rect ? rect.left : 0;
    }
    get offsetWidth() {
      const rect = nodeRect(this.windowId, this.viewTag);
      return rect ? rect.width : 0;
    }
    get offsetHeight() {
      const rect = nodeRect(this.windowId, this.viewTag);
      return rect ? rect.height : 0;
    }
    matches() { return false; }
    isConnected() { return true; }
    // Native event registration surface used by the builtin components.  The
    // appservice hit test already synthesizes touch/tap events; these hooks
    // exist so `attached()` does not throw (e.g. `wx-swiper` registers an
    // onChangeEvent callback in `attached`).  Stored for the native channels
    // that get wired up later.
    onChangeEvent(callback) { this.changeEventCallback = callback; }
    onScrollEvent(callback) { this.scrollEventCallback = callback; }
    onLoadEvent(callback) { this.loadEventCallback = callback; }
    onErrorEvent(callback) { this.errorEventCallback = callback; }
    onKeyboardValueChange(callback) { this.keyboardValueChangeCallback = callback; }
    onKeyboardShow(callback) { this.keyboardShowCallback = callback; }
    onKeyboardHeightChange(callback) { this.keyboardHeightChangeCallback = callback; }
    onKeyboardConfirm(callback) { this.keyboardConfirmCallback = callback; }
    onKeyboardComplete(callback) { this.keyboardCompleteCallback = callback; }
    getOffsetPosition(callback) {
      const rect = nodeRect(this.windowId, this.viewTag);
      if (typeof callback === 'function') {
        callback({ left: rect ? rect.left : 0, top: rect ? rect.top : 0 });
      }
    }
    setEventDefaultPrevented() {}
    setModelBindingStat() {}
    associateValue() {}
    markAsListItem() {}
    markAsRepaintBoundary() {}
    preventSnapshot() {}
    setTouchEventNeedsLocalCoords() {}
    setLayoutCallback(callback) { this.layoutCallback = callback; }
    setIntersectionListener() {}
    createIntersectionObserver() { return { disconnect() {} }; }
    getBeforePseudoNode() { return null; }
    getAfterPseudoNode() { return null; }
    updateStyle(value) { this.setStyle(value); }
    appendStyle(value) { this.setStyle(`${this._style}${value}`); }
    clearAnimatedStyle() {}
    appendFragment(fragment) {
      if (!fragment) return;
      for (const child of fragment.children) this.appendChild(child);
    }
    insertFragmentBefore(fragment, before) {
      if (!fragment) return;
      const index = this.children.indexOf(before);
      const position = index < 0 ? this.children.length : index;
      const children = fragment.children.slice();
      for (let i = 0; i < children.length; i += 1) {
        this.insertBefore(children[i], this.children[position + i] || null);
      }
    }
    spliceBefore(position, count, ...items) { this.splice(position, count, ...items); }
    spliceAppend(...items) { this.splice(this.children.length, 0, ...items); }
    spliceRemove(position, count) { this.splice(position, count); }
    // Component-specific ShadowNode methods.  ScrollView is the default page
    // node, so glass-easel calls these before any child is created; unknown
    // scroll behaviours are inert until the native scroll pipeline needs them.
    setRefresherHeader() {}
    scrollIntoView(...args) {
      if (typeof native.log === 'function') {
        native.log(`scrollIntoView id=${this.viewTag} args=${debugArgs(args)}`);
      }
    }
    scrollIntoViewWithinExtent(...args) {
      if (typeof native.log === 'function') {
        native.log(`scrollIntoViewWithinExtent id=${this.viewTag} args=${debugArgs(args)}`);
      }
    }
    scrollIntoViewAlignment(...args) {
      if (typeof native.log === 'function') {
        native.log(`scrollIntoViewAlignment id=${this.viewTag} args=${debugArgs(args)}`);
      }
    }
    scrollIntoViewOffset(...args) {
      if (typeof native.log === 'function') {
        native.log(`scrollIntoViewOffset id=${this.viewTag} args=${debugArgs(args)}`);
      }
    }
    scrollWithAnimation(...args) {
      if (typeof native.log === 'function') {
        native.log(`scrollWithAnimation id=${this.viewTag} args=${debugArgs(args)}`);
      }
    }
    holdPosition(...args) {
      if (typeof native.log === 'function') {
        native.log(`holdPosition id=${this.viewTag} args=${debugArgs(args)}`);
      }
    }
    requestRefresherRefresh() {}
    requestRefresherTwoLevel() {}
    getScrollPosition() { return 0; }
    onScrollStartEvent() {}
    onScrollEvent() {}
    onScrollEndEvent() {}
    onScrollToLowerEvent() {}
    onScrollToUpperEvent() {}
    onRefresherPullingEvent() {}
    onRefresherWillRefreshEvent() {}
    onRefresherRefreshEvent() {}
    onRefresherRestoreEvent() {}
    onRefresherAbortEvent() {}
    onRefresherStatusChangeEvent() {}
    release() {
      this.ops.push({ op: 'release', id: this.viewTag });
    }
  }

  class BridgePageContext {
    constructor(id, windowId) {
      this.id = id;
      this.windowId = windowId;
      this.ops = bridgeOps(windowId);
      this.sent = false;
      this.flushing = false;
      this.flushTimer = null;
      this.rootNode = null;
      this.rendered = false;
      this.frameworkType = 'glass-easel';
      liveContexts.add(this);
      // Selector-query viewport rect reads these (see WASubContext's
      // SelectorQuery for `selectViewport()`).
      const size = windowSize(windowId);
      this.clientWidth = size ? size.width : 0;
      this.clientHeight = size ? size.height : 0;
      this.ops.push({ op: 'createContext', id, windowId });
      if (typeof native.log === 'function') {
        native.log(`PageContext(${id}) window=${windowId}`);
      }
    }
    createElement(tag, name) {
      const node = new BridgeElement(tag, name, this.ops, this.windowId);
      // The root-portal adapter creates a detached overlay root that the native
      // runtime hoists to the page root; the JS hit test must visit it too.
      if (name === 'root-portal-content' || name === 'rootportalcontent') {
        let portals = detachedPortals.get(this.windowId);
        if (!portals) {
          portals = [];
          detachedPortals.set(this.windowId, portals);
        }
        portals.push(node);
        if (typeof native.log === 'function') {
          native.log(`tracked detached portal ${name} window=${this.windowId} ` +
            `id=${node.viewTag} count=${portals.length}`);
        }
      }
      return node;
    }
    createTextNode(text) {
      const node = new BridgeElement('text', '', this.ops, this.windowId);
      node.setText(text);
      return node;
    }
    createFragment(name) {
      return new BridgeElement('fragment', name === undefined ? 'fragment' : name,
                               this.ops, this.windowId);
    }
    appendStyleSheet(css, index) {
      this.ops.push({ op: 'appendStyleSheet', index, css: String(css) });
    }
    // glass-easel backend style API.  The native replay consumes plain CSS, so
    // text stylesheets are forwarded; compiled/binary stylesheets are recorded
    // as diagnostics until the native CSS path understands them.
    appendStyleSheets(list) {
      for (const sheet of list || []) {
        const content = sheet && (sheet.content || sheet.css);
        if (content) this.ops.push({ op: 'appendStyleSheet', index: 1, css: String(content) });
      }
    }
    appendCompiledStyleSheet(scopeId, compiled) {
      const css = compiledStyleSheets.get(compiled);
      if (typeof native.log === 'function') {
        native.log(`compiledStyleSheet scope=${scopeId} id=${compiled} ` +
          `css=${css ? css.length : 'none'}`);
      }
      if (css) {
        // The native replay consumes plain CSS; `index` is currently unused by
        // the runtime but kept for the recorded fixture schema.
        this.ops.push({ op: 'appendStyleSheet', index: scopeId, css });
      }
    }
    appendCompiledStyleSheets(list) {
      const paths = [];
      for (const sheet of list || []) {
        if (sheet && sheet.path) paths.push(sheet.path);
      }
      if (paths.length && typeof native.log === 'function') {
        native.log(`compiledStyleSheets ${paths.slice(0, 4).join(',')}${paths.length > 4 ? ',...' : ''}`);
      }
    }
    clearStylesheets() {}
    finishStyleSheetsCompilation() {}
    preCompileStyleSheets(list) { return list; }
    appendStyleSheetIndex() {}
    createStyleSheetIndexGroup() { return 0; }
    getRootNode() {
      if (this.rootNode === null) {
        this.rootNode = new BridgeElement('empty', 'root', this.ops, this.windowId);
      }
      return this.rootNode;
    }
    getHostNode() {
      return this.getRootNode();
    }
    getWindowId() { return this.windowId; }
    attach(root, host) {
      this.ops.push({ op: 'attach', root: bridgeId(root), host: bridgeId(host) });
      if (root && typeof root === 'object') {
        attachedRoots.set(this.windowId, root);
        let stack = attachedRootStack.get(this.windowId);
        if (!stack) {
          stack = [];
          attachedRootStack.set(this.windowId, stack);
        }
        if (stack.length === 0 || stack[stack.length - 1] !== root) {
          stack.push(root);
        }
      }
      // `startRender` marks the tree as complete and flushes synchronously.
      // Keep a long fallback for bundles that only call attach; a short timer
      // here flushes before the glass-easel tree has finished building.
      if (this.flushTimer === null) {
        this.flushTimer = setTimeout(() => { this.flushTimer = null; this.flush(); }, 2000);
      }
    }
    attachCustomRoute(...args) {
      this.ops.push({ op: 'attachCustomRoute', args: args.map(bridgeScalar) });
    }
    startRender(callback) {
      this.ops.push({ op: 'startRender' });
      this.rendered = true;
      this.flush();
      if (typeof callback === 'function') setTimeout(callback, 0);
    }
    setAsTab() {
      this.ops.push({ op: 'setAsTab' });
    }
    // Remaining PageContext surface used by the mini program SDK/Dart engine.
    render() { this.flush(); }
    release() { liveContexts.delete(this); }
    recalcStyle() {}
    updateRouteConfig() {}
    setNavigateBackInterception() {}
    forceSecondaryAnimation() {}
    multi() {}
    getNodeFromPoint() { return 0; }
    getComputedStyle() { return {}; }
    dispatchEvent() {}
    sendMessage() {}
    loadFontFace() {}
    showDomHighLighter() {}
    hideDomHighLighter() {}
    dump() { return ''; }
    decodeImage() { return 0; }
    measureText(texts, minWidths, maxWidths, styles, callback) {
      if (typeof callback === 'function') {
        const list = Array.isArray(texts) ? texts : [];
        try {
          callback(list.map(() => 0));
        } catch (_error) {}
      }
    }
    flush() {
      // A page can render repeatedly (route changes, setData, reLaunch); keep
      // flushing the accumulated ops instead of latching after the first send.
      if (this.flushing || this.ops.length === 0) return;
      this.flushing = true;
      if (this.flushTimer !== null) {
        clearTimeout(this.flushTimer);
        this.flushTimer = null;
      }
      const count = this.ops.length;
      const kinds = this.ops.slice(0, 8).map((op) => op.op).join(',');
      let payload;
      try {
        payload = serializeOps(this.ops);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`PageContext.flush serialize failed window=${this.windowId} ops=${count} error=${error && error.message}`);
        }
        this.flushing = false;
        return;
      }
      this.ops.length = 0;
      if (typeof native.log === 'function') {
        native.log(`PageContext.flush window=${this.windowId} ops=${count} kinds=${kinds}`);
      }
      try {
        if (typeof native.applyPageOps === 'function') {
          native.applyPageOps(this.windowId, payload);
        }
      } finally {
        this.flushing = false;
      }
    }
  }

  // -- appservice DOM touch dispatch -----------------------------------------
  // Skyline RuntimeCore calls runtime.registerTouchCallback(cb) and expects the
  // native runtime to feed it native hit-test results.  The element tree and
  // viewport geometry are mirrored on this side, so hit-test the tree and
  // invoke the callback with the glass-easel element (`__wxElement`).
  const runtimeTouchCallbacks = [];
  const runtimeMouseCallbacks = [];
  const runtimeTransitionCallbacks = [];
  const runtimeTriggerEventCallbacks = [];
  const attachedRoots = new Map();
  // Attached roots in page-stack order (skyline keeps covered pages alive
  // while navigateTo pushes a new route).  navigateBack restores the previous
  // root here as well so the JS hit test follows the visible page.
  const attachedRootStack = new Map();
  const detachedPortals = new Map();
  const lastTouchPoint = new Map();
  // Debug handle for CDP inspection of the mirrored element tree.
  globalThis.__skylineDebug = {
    attachedRoots, detachedPortals, lastTouchPoint,
    touchCallbacks: runtimeTouchCallbacks,
    rect: nodeRect,
    hit: hitTestElement,
    scrollTo(windowId, nodeId, top) {
      if (typeof native.log === 'function') {
        native.log(`scroll-into-view node=${nodeId} top=${top}`);
      }
      if (typeof native.scrollNodeTo === 'function') {
        native.scrollNodeTo(windowId, nodeId, top);
      }
    },
    log(message) {
      if (typeof native.log === 'function') native.log(String(message));
    },
  };

  function pointInRect(rect, x, y) {
    return !!rect && rect.width >= 0 && rect.height >= 0 &&
      x >= rect.left && x < rect.left + rect.width &&
      y >= rect.top && y < rect.top + rect.height;
  }

  function hitTestElement(windowId, node, x, y) {
    if (!node || node.released) return null;
    // The component host only carries the detached overlay; it is not a touch
    // target of its own (pointer-events: none semantics).
    if (node.name === 'root-portal' || node.name === 'rootportal') return null;
    if (typeof node._style === 'string' && node._style.indexOf('display:none') >= 0) {
      return null;
    }
    // Visit siblings in paint order (ascending z-index, then DOM order) from
    // top to bottom so a navigation bar stacked above a full-screen image wins
    // the hit test even though it comes first in the tree.
    const candidates = [];
    for (let i = 0; i < node.children.length; i += 1) {
      const child = node.children[i];
      if (!child) continue;
      if (child.name === 'root-portal' || child.name === 'rootportal') continue;
      const rect = nodeRect(windowId, child.viewTag);
      if (!rect) continue;
      candidates.push({
        child,
        index: i,
        rect,
        z: typeof rect.zIndex === 'number' ? rect.zIndex : 0,
      });
    }
    candidates.sort((a, b) => (a.z - b.z) || (a.index - b.index));
    for (let i = candidates.length - 1; i >= 0; i -= 1) {
      const { child, rect } = candidates[i];
      const inside = pointInRect(rect, x, y);
      // A zero-area box (e.g. an auto-sized `position:absolute` wrapper whose
      // children are absolutely positioned, like the previewer's navigation
      // bar) does not clip its overflow; descendants can still be hit outside
      // its own border box.  Non-empty boxes keep the clipping approximation.
      if (!inside && (rect.width > 0 && rect.height > 0)) continue;
      const deeper = hitTestElement(windowId, child, x, y);
      if (deeper) return deeper;
      // Containers do not swallow touches when none of their children hit;
      // this mirrors the patched YGConnector shadow hit test.
      if (inside && !child.children.length) return child;
    }
    return null;
  }

  function touchTargetElement(windowId, x, y) {
    const roots = [];
    const portals = detachedPortals.get(windowId);
    if (portals) {
      for (let i = portals.length - 1; i >= 0; i -= 1) roots.push(portals[i]);
    }
    const attached = attachedRoots.get(windowId);
    if (attached) roots.push(attached);
    for (const root of roots) {
      let node = hitTestElement(windowId, root, x, y);
      // Text backend nodes may not carry the exparser back-reference; dispatch
      // to the nearest ancestor element instead.
      while (node && !node.__wxElement) node = node.parent;
      if (node && node.__wxElement) return node.__wxElement;
    }
    return null;
  }

  function makeTouch(target, x, y) {
    return {
      identifier: 0, target,
      pageX: x, pageY: y, clientX: x, clientY: y,
      screenX: x, screenY: y, x, y,
      radiusX: 1, radiusY: 1,
      rotationAngle: 0, force: 1,
    };
  }

  // -- scroll-into-view ------------------------------------------------------
  // The WXML `scroll-into-view` property is consumed by the appservice
  // scroll-view component (it briefly sets the component property, then resets
  // it) but the DevTools has no renderer-side handler that turns it into a
  // native scroll.  The component tree lives in the app_sub_render realm, so
  // install a small monitor there that watches the owning component's
  // `intoView` data and drives the materialized widget through the addon.
  let scrollMonitorInstalled = false;

  const SCROLL_MONITOR_SCRIPT = `(function(){
    const parentWindow = window.parent;

    function backendOf(element) {
      return element && (element._$backendElement || element.__domElement || null);
    }

    function composedWalk(element, visit) {
      if (!element) return;
      visit(element);
      let children = null;
      try {
        const shadow = element.getShadowRoot ? element.getShadowRoot() : null;
        children = (shadow && shadow.childNodes) || element.childNodes || null;
      } catch (error) {
        children = null;
      }
      if (!children) return;
      for (const child of children) {
        if (child && child.childNodes) composedWalk(child, visit);
      }
    }

    function findComponentWithIntoView(rootElement) {
      let found = null;
      composedWalk(rootElement, function(element) {
        if (found || !element.getMethodCaller) return;
        if (typeof element.is !== 'string' || element.is.indexOf('/') < 0) return;
        try {
          const caller = element.getMethodCaller();
          const data = caller && caller.data;
          if (data && typeof data.intoView === 'string') {
            found = { element, intoView: data.intoView };
          }
        } catch (error) {}
      });
      return found;
    }

    // The scroll content (sticky headers, list slivers) lives in the mirrored
    // backend tree, not in the composed shadow tree exposed by the scroll-view
    // native component.
    function findScrollBackend(root) {
      if (!root) return null;
      // Prefer the adapter scrollview node: it owns the materialized Flutter
      // scroll widget, while the wx-x scroll-view node is its plain wrapper.
      if (root.name === 'scrollview') return root;
      for (const child of root.children) {
        const found = findScrollBackend(child);
        if (found) return found;
      }
      return null;
    }

    function findTargetBackend(root, id) {
      if (!root) return null;
      const nodeId = root._id || '';
      if (nodeId === id || nodeId.endsWith('--' + id)) return root;
      for (const child of root.children) {
        const found = findTargetBackend(child, id);
        if (found) return found;
      }
      return null;
    }

    const seen = new Map();
    setInterval(function() {
      try {
        const debug = parentWindow.__skylineDebug;
        if (!debug || !debug.attachedRoots || !debug.rect || !debug.scrollTo) return;
        for (const entry of debug.attachedRoots) {
          const windowId = entry[0];
          const root = entry[1];
          const rootElement = root && root.__wxElement;
          if (!rootElement) continue;
          const component = findComponentWithIntoView(rootElement);
          if (!component || !component.intoView) continue;
          const key = windowId + ':' + (backendOf(component.element)
            ? backendOf(component.element).viewTag : component.element.is);
          if (seen.get(key) === component.intoView) continue;
          seen.set(key, component.intoView);
          const scrollBackend = findScrollBackend(root);
          const targetBackend = findTargetBackend(root, component.intoView);
          if (!scrollBackend || !targetBackend) continue;
          const scrollRect = debug.rect(windowId, scrollBackend.viewTag);
          const targetRect = debug.rect(windowId, targetBackend.viewTag);
          if (!scrollRect || !targetRect) continue;
          debug.scrollTo(windowId, scrollBackend.viewTag,
                         Math.max(0, targetRect.top - scrollRect.top));
        }
      } catch (error) {}
    }, 16);
  })();`;

  function installRenderScrollMonitor() {
    try {
      const maps = typeof window !== 'undefined' && window.__global &&
        window.__global.contextFrameMaps;
      if (!maps || typeof maps.forEach !== 'function') return false;
      let renderWindow = null;
      maps.forEach((frame) => {
        if (frame && frame.name === 'app_sub_render' && frame.iframe) {
          try {
            if (frame.iframe.contentWindow) renderWindow = frame.iframe.contentWindow;
          } catch (_error) {}
        }
      });
      if (!renderWindow) return false;
      if (renderWindow.__skylineScrollMonitor) return true;
      renderWindow.eval(SCROLL_MONITOR_SCRIPT);
      renderWindow.__skylineScrollMonitor = true;
      if (typeof native.log === 'function') {
        native.log('scroll-into-view monitor installed');
      }
      return true;
    } catch (_error) {
      return false;
    }
  }

  function emitRuntimeTouch(event) {
    for (const callback of runtimeTouchCallbacks) {
      try {
        callback(event);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`touch callback failed: ${error && error.message}`);
        }
      }
    }
  }

  function dispatchDomTouchEvent(windowId, type, x, y) {
    if (!runtimeTouchCallbacks.length) return;
    if (type === 'touchcancel') {
      const previous = lastTouchPoint.get(windowId);
      if (previous) { x = previous.x; y = previous.y; }
    }
    if (type === 'touchstart') {
      const target = touchTargetElement(windowId, x, y);
      const started = { x, y, target, time: Date.now(), moved: false };
      lastTouchPoint.set(windowId, started);
      if (!target) return;
      const touch = makeTouch(target, x, y);
      emitRuntimeTouch({
        type: 'touchstart', target, touches: [touch], changedTouches: [touch],
        timeStamp: Date.now(), bubbles: true, cancelable: true,
      });
      return;
    }
    const state = lastTouchPoint.get(windowId);
    if (!state || !state.target) return;
    if (type === 'touchmove') {
      if (Math.abs(x - state.x) > 10 || Math.abs(y - state.y) > 10) state.moved = true;
      const touch = makeTouch(state.target, x, y);
      emitRuntimeTouch({
        type: 'touchmove', target: state.target, touches: [touch],
        changedTouches: [touch], timeStamp: Date.now(), bubbles: true,
        cancelable: true,
      });
      return;
    }
    const touch = makeTouch(state.target, x, y);
    emitRuntimeTouch({
      type, target: state.target, touches: [], changedTouches: [touch],
      timeStamp: Date.now(), bubbles: true, cancelable: true,
    });
    const isTap = type === 'touchend' && !state.moved &&
      Date.now() - state.time < 700;
    lastTouchPoint.delete(windowId);
    if (isTap) {
      emitRuntimeTouch({
        type: 'tap', target: state.target, touches: [],
        changedTouches: [touch], timeStamp: Date.now(), bubbles: true,
        cancelable: true,
      });
    }
  }

  const bridgeRuntime = autoStub({
    registerJsValue(value) {
      const id = ++nextJsValueId;
      jsValues.set(id, value);
      return id;
    },
    getJsValueById(id) { return jsValues.get(id); },
    unRegisterJsValue(id) { jsValues.delete(id); },
    // The Skyline style runtime asks the native runtime to compile a WXSS
    // resource and returns an opaque handle.  We load the resource through the
    // DevTools' registered loader and keep the text so appendCompiledStyleSheet
    // can replay it through the native CSS parser.
    compileStyleSheet(path, groupId, prefix) {
      const id = ++nextStyleSheetId;
      let data = null;
      let error = null;
      if (typeof shellCallbacks.setLoadResourceCallback === 'function') {
        try {
          data = shellCallbacks.setLoadResourceCallback({ path });
        } catch (loadError) {
          error = loadError;
        }
      }
      let text = toText(data);
      if (!looksLikeCss(text)) {
        const fetched = loadStyleSheetText(path);
        if (looksLikeCss(fetched)) text = fetched;
      }
      if (looksLikeCss(text)) compiledStyleSheets.set(id, text);
      if (typeof native.log === 'function') {
        native.log(`compileStyleSheet path=${path} group=${groupId} prefix=${prefix} ` +
          `id=${id} ${error ? 'error=' + error.message : describeData(text, text)}`);
      }
      return id;
    },
    registerEventCallback(callback) {
      if (typeof callback === 'function') runtimeTriggerEventCallbacks.push(callback);
    },
    registerTriggerEventCallback(callback) {
      if (typeof callback === 'function') runtimeTriggerEventCallbacks.push(callback);
    },
    registerAnimationCallback() {},
    registerTouchCallback(callback) {
      if (typeof callback === 'function') runtimeTouchCallbacks.push(callback);
    },
    registerKeyCallback() {},
    registerMouseCallback(callback) {
      if (typeof callback === 'function') runtimeMouseCallbacks.push(callback);
    },
    registerTransitionCallback(callback) {
      if (typeof callback === 'function') runtimeTransitionCallbacks.push(callback);
    },
  });
  const bridgeNavigator = autoStub({
    notifyNavigateTo(...args) { bridgeOps(0).push({ op: 'notifyNavigateTo', args }); },
    notifySwitchTab(...args) { bridgeOps(0).push({ op: 'notifySwitchTab', args }); },
    notifyNavigateBack(...args) { bridgeOps(0).push({ op: 'notifyNavigateBack', args }); },
  });
  const SkylineGlobal = {
    runtime: bridgeRuntime,
    Runtime: bridgeRuntime,
    navigator: bridgeNavigator,
    Navigator: bridgeNavigator,
    PageContext: BridgePageContext,
    TabBarContext: BridgePageContext,
    AppBarContext: BridgePageContext,
    features: autoStub({}),
    gestureHandlerModule: workletStub(),
    workletModule: workletStub(),
    ReanimatedModule: workletStub(),
  };

  const debugInfo = native.debugInfo();
  // Environment probe: the addon's script may run in a different V8 context
  // from the page world, which decides whether the appservice's context maps
  // (and event bus) are reachable from the bootstrap.
  if (typeof native.log === 'function') {
    const hasWindow = typeof window !== 'undefined';
    const hasGlobal = typeof globalThis !== 'undefined';
    let pageGlobal = 'n/a';
    let globalGlobal = 'n/a';
    let contextMaps = 'n/a';
    try { pageGlobal = hasWindow ? typeof window.__global : 'no-window'; } catch (_e) { pageGlobal = 'err'; }
    try { globalGlobal = hasGlobal ? typeof globalThis.__global : 'no-global'; } catch (_e) { globalGlobal = 'err'; }
    try {
      const maps = hasWindow && window.__global ? window.__global.contextFrameMaps
        : (hasGlobal ? globalThis.__global && globalThis.__global.contextFrameMaps : null);
      contextMaps = maps ? (typeof maps.forEach === 'function' ? 'map' : typeof maps) : 'none';
    } catch (_e) { contextMaps = 'err'; }
    native.log(`env window=${hasWindow} globalThis=${hasGlobal} ` +
      `same=${hasWindow && hasGlobal && window === globalThis} ` +
      `pageGlobal=${pageGlobal} globalGlobal=${globalGlobal} ` +
      `contextMaps=${contextMaps} wjb=${typeof WeixinJSBridge !== 'undefined' ? 'yes' : 'no'}`);
  }
  // The official PageContext applies each mutation immediately; our bridge
  // batches ops until an explicit flush.  The miniprogram SDK does not call
  // render()/flush() for ordinary setData updates, so coalesce them here.
  const liveFlushTimer = setInterval(() => {
    for (const context of liveContexts) {
      if (context.rendered && context.ops.length > 0) {
        try {
          context.flush();
        } catch (_error) {}
      }
    }
    if (!scrollMonitorInstalled) {
      scrollMonitorInstalled = installRenderScrollMonitor();
    }
  }, 16);
  if (liveFlushTimer && typeof liveFlushTimer.unref === 'function') {
    liveFlushTimer.unref();
  }

  installWorkletUIModules();
  const workletModuleTimer = setInterval(installWorkletUIModules, 100);
  setTimeout(() => {
    clearInterval(workletModuleTimer);
    installWorkletUIModules();
  }, 20000);

  const gateInstalled = installCreateWindowResponseGate();
  if (typeof native.log === 'function') {
    native.log(`createWindow response gate installed=${gateInstalled}`);
  }
  // Electron loads native modules through the preload/native bridge; the
  // DevTools extensions read these globals from the page world, so publish
  // them on window/globalThis as well as the module's own global.
  const targets = [];
  if (typeof globalThis !== 'undefined') targets.push(globalThis);
  if (typeof window !== 'undefined' && window !== globalThis) targets.push(window);
  if (typeof global !== 'undefined' && global !== globalThis && global !== window) {
    targets.push(global);
  }
  for (const target of targets) {
    target.SkylineShell = SkylineShell;
    target.SkylineDebugInfo = debugInfo;
    target.SkylineGlobal = SkylineGlobal;
  }
})();

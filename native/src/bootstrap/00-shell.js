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
      // SkylineJS gesture handlers (`<pan-gesture-handler>` & friends) consume
      // the same pointer stream as the appservice DOM events.
      try {
        dispatchGestureTouch(windowId, kind, x, y);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`gesture dispatch failed: ${error && error.message}`);
        }
      }
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

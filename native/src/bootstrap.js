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
        native.log(`initial route created ${pages} page(s)`);
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
          native.log(`retry initial route via reLaunch ${url}`);
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
      const created = native.createWindow(args);
      const windowId = args[0];
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
    ['notifyNavigateTo', function (...args) {
      native.notify('notifyNavigateTo', args);
      if (typeof native.log === 'function') {
        native.log(`notifyNavigateTo window=${args[0]} page=${args[1]} task=${args[2]}`);
      }
      completeRoute(args[2]);
      return undefined;
    }],
    ['notifyRedirectTo', function (...args) {
      native.notify('notifyRedirectTo', args);
      if (typeof native.log === 'function') {
        native.log(`notifyRedirectTo window=${args[0]} page=${args[1]} task=${args[2]}`);
      }
      completeRoute(args[2]);
      return undefined;
    }],
    ['notifyNavigateBack', function (...args) {
      native.notify('notifyNavigateBack', args);
      if (typeof native.log === 'function') {
        native.log(`notifyNavigateBack window=${args[0]} page=${args[1]} task=${args[4]}`);
      }
      completeRoute(args[4]);
      return undefined;
    }],
    ['notifyAppLaunch', function (...args) {
      native.notify('notifyAppLaunch', args);
      if (typeof native.log === 'function') {
        native.log(`notifyAppLaunch window=${args[0]} page=${args[1]}`);
      }
      // Complete the DevTools appLaunch handshake so the appservice proceeds
      // to build its PageContext tree (the native page is created when that
      // tree is attached).  appLaunch routes always use taskId 0.
      completeRoute(0);
      // Fallback: if the launch route initialised the app but produced no
      // page, replay it once through the SDK after the route has settled.
      setTimeout(() => retryLaunchIfNoPage(0), 1500);
      return undefined;
    }],
    ['notifyReLaunch', function (...args) {
      native.notify('notifyReLaunch', args);
      if (typeof native.log === 'function') {
        native.log(`notifyReLaunch window=${args[0]} page=${args[1]}`);
      }
      completeRoute(0);
      return undefined;
    }],
    ['notifyAutoReLaunch', function (...args) {
      native.notify('notifyAutoReLaunch', args);
      completeRoute(0);
      return undefined;
    }],
    ['notifySwitchTab', function (...args) {
      native.notify('notifySwitchTab', args);
      if (typeof native.log === 'function') {
        native.log(`notifySwitchTab window=${args[0]} page=${args[1]}`);
      }
      completeRoute(0);
      return undefined;
    }],
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
  function workletStub() {
    const target = Object.create(null);
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        const fn = function (value) {
          return typeof value === 'function' ? value : undefined;
        };
        object[property] = fn;
        return fn;
      },
    });
  }

  class BridgeElement {
    constructor(tag, name, ops) {
      this.ops = ops || [];
      this.tag = tag;
      this.name = name;
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
    getBoundingClientRect() {
      return { left: 0, top: 0, width: 0, height: 0, right: 0, bottom: 0 };
    }
    getAttribute(name) { return this.attrs[name]; }
    cloneNode() {
      const clone = new BridgeElement(this.tag, this.name, this.ops);
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
    matches() { return false; }
    isConnected() { return true; }
    getOffsetPosition(callback) {
      if (typeof callback === 'function') callback({ left: 0, top: 0 });
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
    scrollIntoView() {}
    scrollIntoViewWithinExtent() {}
    scrollIntoViewAlignment() {}
    scrollIntoViewOffset() {}
    scrollWithAnimation() {}
    holdPosition() {}
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
      this.frameworkType = 'glass-easel';
      this.clientWidth = 0;
      this.clientHeight = 0;
      this.ops.push({ op: 'createContext', id, windowId });
      if (typeof native.log === 'function') {
        native.log(`PageContext(${id}) window=${windowId}`);
      }
    }
    createElement(tag, name) {
      return new BridgeElement(tag, name, this.ops);
    }
    createTextNode(text) {
      const node = new BridgeElement('text', '', this.ops);
      node.setText(text);
      return node;
    }
    createFragment(name) {
      return new BridgeElement('fragment', name === undefined ? 'fragment' : name, this.ops);
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
      if (typeof native.log === 'function') {
        native.log(`compiledStyleSheet scope=${scopeId} size=${compiled && compiled.byteLength !== undefined ? compiled.byteLength : typeof compiled}`);
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
        this.rootNode = new BridgeElement('empty', 'root', this.ops);
      }
      return this.rootNode;
    }
    getHostNode() {
      return this.getRootNode();
    }
    getWindowId() { return this.windowId; }
    attach(root, host) {
      this.ops.push({ op: 'attach', root: bridgeId(root), host: bridgeId(host) });
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
      this.flush();
      if (typeof callback === 'function') setTimeout(callback, 0);
    }
    setAsTab() {
      this.ops.push({ op: 'setAsTab' });
    }
    // Remaining PageContext surface used by the mini program SDK/Dart engine.
    render() { this.flush(); }
    release() {}
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

  const bridgeRuntime = autoStub({
    registerJsValue(value) {
      const id = ++nextJsValueId;
      jsValues.set(id, value);
      return id;
    },
    getJsValueById(id) { return jsValues.get(id); },
    unRegisterJsValue(id) { jsValues.delete(id); },
    registerEventCallback() {},
    registerAnimationCallback() {},
    registerTouchCallback() {},
    registerKeyCallback() {},
    registerMouseCallback() {},
    registerTransitionCallback() {},
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

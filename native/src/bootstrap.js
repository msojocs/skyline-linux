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

  function setter(method) {
    return function (...args) {
      if (args.length !== 1 || typeof args[0] !== 'function') {
        throw new Error(`${'Set' + method.slice(3)} expects a function argument`);
      }
      shellCallbacks[method] = args[0];
      native.setCallback(method, args[0]);
      if (method === 'setNotifyBootstrapDoneCallback') {
        // The engine is ready as soon as the module loads; the DevTools gate
        // for creating the first window waits on this notification.
        setTimeout(() => {
          if (typeof shellCallbacks.setNotifyBootstrapDoneCallback === 'function') {
            shellCallbacks.setNotifyBootstrapDoneCallback();
          }
        }, 0);
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
      const created = native.createWindow(args);
      const windowId = args[0];
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
    ['notifyNavigateTo', function (...args) { return native.notify('notifyNavigateTo', args); }],
    ['notifyRedirectTo', function (...args) { return native.notify('notifyRedirectTo', args); }],
    ['notifyNavigateBack', function (...args) { return native.notify('notifyNavigateBack', args); }],
    ['notifyAppLaunch', function (...args) {
      native.notify('notifyAppLaunch', args);
      const windowId = args[0];
      // Complete the DevTools appLaunch handshake so the appservice proceeds
      // to build its PageContext tree (the native page is created when that
      // tree is attached).
      setTimeout(() => {
        if (typeof shellCallbacks.setNotifyRouteDoneCallback === 'function') {
          shellCallbacks.setNotifyRouteDoneCallback(windowId);
        }
      }, 50);
      return undefined;
    }],
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

  function autoStub(target) {
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        return () => ++nextBridgeTag;
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
      this.ops.push({ op: 'replaceChild', id: this.viewTag, child: bridgeId(child), index });
      this.children[index] = child;
      child.parent = this;
    }
    findChildPosition(child) { return this.children.indexOf(child); }
    splice(position, count, ...items) {
      this.ops.push({
        op: 'splice', id: this.viewTag, pos: position, count,
        items: items.map(bridgeId),
      });
      if (position < 0 || position > this.children.length) position = this.children.length;
      const flat = [];
      for (const item of items) {
        if (item && item.tag === 'fragment') flat.push(...item.children);
        else if (item !== undefined && item !== null) flat.push(item);
      }
      this.children.splice(position, count, ...flat);
      for (const child of flat) child.parent = this;
    }
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
      this.flushTimer = null;
      this.ops.push({ op: 'createContext', id, windowId });
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
    attach(root, host) {
      this.ops.push({ op: 'attach', root: bridgeId(root), host: bridgeId(host) });
      // Some bundles only call startRender after attach; give it a moment so the
      // flush includes the final operations.
      if (this.flushTimer === null) {
        this.flushTimer = setTimeout(() => { this.flushTimer = null; this.flush(); }, 50);
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
    flush() {
      if (this.sent || this.ops.length === 0) return;
      this.sent = true;
      if (this.flushTimer !== null) {
        clearTimeout(this.flushTimer);
        this.flushTimer = null;
      }
      const payload = JSON.stringify({ ops: this.ops.slice() });
      this.ops.length = 0;
      if (typeof native.applyPageOps === 'function') {
        native.applyPageOps(this.windowId, payload);
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
    gestureHandlerModule: autoStub({}),
    workletModule: autoStub({}),
    ReanimatedModule: autoStub({}),
  };

  const debugInfo = native.debugInfo();
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

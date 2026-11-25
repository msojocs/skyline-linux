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
    // Called by the app-bar adapter right before it attaches its context.  The
    // attach itself is recorded by BridgeAppBarContext; the runtime installs
    // the built tree as a window overlay.
    attachAppBar() { return 0; },
    removeAppBarContext() { return 0; },
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
    AppBarContext: BridgeAppBarContext,
    features: autoStub({}),
    GestureHandlerModule: createGestureHandlerModule(),
    gestureHandlerModule: autoStub({}),
    workletModule: workletStub(),
    ReanimatedModule: workletStub(),
  };
  // SkylineJS destructures `GestureHandlerModule`; older bundles use the
  // lower-case alias.
  SkylineGlobal.gestureHandlerModule = SkylineGlobal.GestureHandlerModule;

  // The runtime forwards real ScrollViewUpdateOnScroll/Start/End callbacks
  // through this sink.
  if (typeof native.setScrollSink === 'function') {
    native.setScrollSink(function (event) {
      try {
        dispatchNativeScrollEvent(event);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`native scroll dispatch failed: ${error && error.message}`);
        }
      }
    });
  }
  if (typeof native.setWidgetEventSink === 'function') {
    native.setWidgetEventSink(function (event) {
      try {
        dispatchNativeWidgetEvent(event);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`native widget dispatch failed: ${error && error.message}`);
        }
      }
    });
  }
  if (typeof native.setJsonEventSink === 'function') {
    native.setJsonEventSink(function (payload) {
      try {
        dispatchNativeJsonEvent(payload);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`native json dispatch failed: ${error && error.message}`);
        }
      }
    });
  }
  // Native image/font HTTP loads are proxied through the DevTools pageframe
  // fetch (`SkylineShell.setHttpRequestCallback`) so the Network panel shows
  // them.  The DevTools callback calls `notifyHttpRequestComplete` with bytes.
  if (typeof native.setHttpRequestSink === 'function') {
    native.setHttpRequestSink(function (request) {
      const requestId = request && request.requestId;
      const url = request && request.url;
      const complete = function (bytes) {
        if (typeof native.httpRequestComplete === 'function') {
          native.httpRequestComplete(requestId, bytes);
        }
      };
      const callback = shellCallbacks.setHttpRequestCallback;
      if (typeof callback !== 'function') {
        complete(new Uint8Array(0));
        return;
      }
      try {
        if (typeof native.log === 'function') {
          native.log(`httpRequest ${requestId} ${url}`);
        }
        const result = callback({ request_id: requestId, url });
        if (result && typeof result.then === 'function') {
          result.catch(function (error) {
            if (typeof native.log === 'function') {
              native.log(`httpRequest ${requestId} failed: ${error && error.message}`);
            }
            complete(new Uint8Array(0));
          });
        }
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`httpRequest ${requestId} threw: ${error && error.message}`);
        }
        complete(new Uint8Array(0));
      }
    });
  }

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
  // Glass-easel runs the builtin share-element component's property observers
  // during init, before its backend element (`$$`) exists.  The observer
  // updates for `transition-on-gesture`, `rect-tween-type` and
  // `shuttle-on-push/pop` are dropped there, so the native Hero would never
  // see the kebab-case WXML props.  The component data still carries the
  // resolved values: forward them once the backend element is linked (and
  // again if a setData changes them).
  function backfillHeroAttributes() {
    for (const element of heroElements.values()) {
      if (!element || element.viewTag == null) continue;
      const wxElement = element.__wxElement;
      const data = wxElement && wxElement.data;
      if (!data) continue;
      const wanted = {};
      if (typeof data.key === 'string' && data.key) wanted.tag = data.key;
      if (typeof data.transitionOnGesture === 'boolean') {
        wanted.transitionOnGesture = data.transitionOnGesture;
      }
      if (typeof data.rectTweenType === 'string') {
        wanted.rectTweenType = data.rectTweenType;
      }
      if (typeof data.shuttleOnPush === 'string' && data.shuttleOnPush) {
        wanted.shuttleOnPush = data.shuttleOnPush;
      }
      if (typeof data.shuttleOnPop === 'string' && data.shuttleOnPop) {
        wanted.shuttleOnPop = data.shuttleOnPop;
      }
      if (!Object.keys(wanted).length) continue;
      const signature = JSON.stringify(wanted);
      if (heroAttributeSignature.get(element) === signature) continue;
      heroAttributeSignature.set(element, signature);
      element.setAttributes(wanted);
    }
  }


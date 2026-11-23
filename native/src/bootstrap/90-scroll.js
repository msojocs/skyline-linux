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

  // Native scroll callbacks (ScrollViewUpdateOnScroll*) arrive on the
  // appservice thread as `{nodeId, phase, scrollTop, ...}`.  Deliver them to
  // whichever registration the builtin scroll-view used: a worklet handler
  // (`onScroll-<tag>`) or the plain JS callback from `bindscroll`.
  function dispatchNativeScrollEvent(event) {
    if (!event) return;
    const tag = event.nodeId;
    if (!tag) return;
    const detail = {
      scrollTop: Number(event.scrollTop) || 0,
      scrollLeft: Number(event.scrollLeft) || 0,
      scrollWidth: Number(event.scrollWidth) || 0,
      scrollHeight: Number(event.scrollHeight) || 0,
      deltaX: Number(event.deltaX) || 0,
      deltaY: Number(event.deltaY) || 0,
    };
    scrollOffsets.set(tag, detail);
    const phase = Number(event.phase) || 0;
    const isStart = phase === 0;
    const isEnd = phase === 2;
    const eventName = isStart ? 'onScrollStart' : (isEnd ? 'onScrollEnd' : 'onScroll');
    const worklet = workletEventHandlers.get(`${eventName}-${tag}`);
    if (worklet) {
      const workletEvent = {
        type: isStart ? 'scrollstart' : (isEnd ? 'scrollend' : 'scroll'),
        detail,
        isDrag: !!event.isDrag,
        boundaryVelocity: Number(event.boundaryVelocity) || 0,
        timeStamp: Date.now(),
      };
      try {
        // Worklet factories capture shared values in `_closure`; calling with
        // it as `this` keeps `this._sharedX` references working on the JS
        // thread fallback.
        worklet.call(worklet._closure || undefined, workletEvent);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`scroll worklet failed: ${error && error.message}`);
        }
      }
    }
    const callbacks = isStart ? scrollStartEventCallbacks
      : (isEnd ? scrollEndEventCallbacks : scrollEventCallbacks);
    const callback = callbacks.get(tag);
    if (callback) {
      try {
        callback(detail.scrollTop, detail.scrollLeft, detail.scrollWidth,
                 detail.scrollHeight, detail.deltaX, detail.deltaY);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`scroll callback failed: ${error && error.message}`);
        }
      }
    }
  }

  function mirrorAncestor(element, names) {
    let node = element ? element.parent : null;
    while (node) {
      if (names.indexOf(node.name) >= 0 || names.indexOf(node.tag) >= 0) {
        return node;
      }
      node = node.parent;
    }
    return null;
  }

  function pickerValues(root) {
    const values = [];
    if (!root) return values;
    for (const child of root.children || []) {
      if (!child) continue;
      const isColumn = child.name === 'picker-view-column' ||
        child.name === 'pickerviewcolumn' ||
        child.tag === 'picker-view-column' ||
        child.tag === 'pickerviewcolumn';
      if (!isColumn) continue;
      const raw = child.attrs && (child.attrs.value ?? child.attrs.current);
      values.push(Number(raw) || 0);
    }
    return values;
  }

  // Offscreen exparser dispatch for native components without a JS builtin
  // (picker-view) and input focus/blur.  Touch events use the RuntimeCore
  // callback instead; these nodes have no such callback.
  let mirrorEventFailures = 0;
  function dispatchMirrorEvent(element, type, detail) {
    const wxElement = element && element.__wxElement;
    if (!wxElement || typeof wxElement.dispatchEvent !== 'function') {
      return false;
    }
    try {
      wxElement.dispatchEvent({
        type,
        detail,
        bubbles: true,
        cancelable: false,
        composed: true,
        timeStamp: Date.now(),
        target: wxElement,
        currentTarget: wxElement,
      });
      return true;
    } catch (error) {
      if (mirrorEventFailures < 5 && typeof native.log === 'function') {
        mirrorEventFailures += 1;
        native.log(`mirror ${type} event failed: ${error && error.message}`);
      }
      return false;
    }
  }

  // Swiper / picker-view-column / input events arrive from the native Dart
  // widgets as `{nodeId, kind, value, extra, flag}`.
  function dispatchNativeWidgetEvent(event) {
    if (!event) return;
    const tag = event.nodeId;
    if (!tag) return;
    const value = Number(event.value) || 0;
    const extra = Number(event.extra) || 0;
    const flag = Number(event.flag) || 0;
    const element = bridgeElements.get(tag);
    switch (Number(event.kind) || 0) {
      case 0: {  // swiper change
        if (element) element.attrs.current = String(value);
        if (element && typeof element.changeEventCallback === 'function') {
          try {
            element.changeEventCallback(value, extra);
          } catch (error) {
            if (typeof native.log === 'function') {
              native.log(`swiper change callback failed: ${error && error.message}`);
            }
          }
        }
        const worklet = workletEventHandlers.get(`onChange-${tag}`);
        if (worklet) {
          try {
            worklet.call(worklet._closure || undefined, {
              type: 'change',
              detail: {
                current: value,
                source: ['touch', 'autoplay', 'manual'][extra] || '',
              },
              timeStamp: Date.now(),
            });
          } catch (error) {
            if (typeof native.log === 'function') {
              native.log(`swiper change worklet failed: ${error && error.message}`);
            }
          }
        }
        return;
      }
      case 1: {  // swiper scroll / transition
        const callback = swiperScrollEventCallbacks.get(tag);
        if (callback) {
          try {
            callback(value);
          } catch (error) {
            if (typeof native.log === 'function') {
              native.log(`swiper scroll callback failed: ${error && error.message}`);
            }
          }
        }
        const vertical = !!(element && element.attrs &&
          (element.attrs.vertical === '1' || element.attrs.vertical === 'true'));
        const dx = vertical ? 0 : value;
        const dy = vertical ? value : 0;
        const stateName = ['onScrollStart', 'onScrollUpdate', 'onScrollEnd'][extra];
        const names = ['onTransition'];
        if (stateName) names.push(stateName);
        for (const name of names) {
          const worklet = workletEventHandlers.get(`${name}-${tag}`);
          if (!worklet) continue;
          try {
            worklet.call(worklet._closure || undefined, {
              type: name === 'onTransition' ? 'transition' : 'scroll',
              detail: { dx, dy, current: flag },
              state: extra,
              timeStamp: Date.now(),
            });
          } catch (error) {
            if (typeof native.log === 'function') {
              native.log(`swiper ${name} worklet failed: ${error && error.message}`);
            }
          }
        }
        return;
      }
      case 2:  // picker column change
      case 3:  // start
      case 4: {  // end
        const type = Number(event.kind) === 2 ? 'change'
          : (Number(event.kind) === 3 ? 'pickstart' : 'pickend');
        if (element) element.attrs.value = String(value);
        const parent = mirrorAncestor(element, ['picker-view', 'pickerview']);
        const detail = { value: pickerValues(parent || element && element.parent) };
        if (type === 'change') detail.column = value;
        pickerEvents.push({
          type, nodeId: tag, value, reason: extra, detail,
        });
        if (parent) dispatchMirrorEvent(parent, type, detail);
        return;
      }
      case 5:   // input focus
      case 6: { // input blur
        const type = Number(event.kind) === 5 ? 'focus' : 'blur';
        inputEvents.push({ type, nodeId: tag });
        if (element) dispatchMirrorEvent(element, type, {});
        return;
      }
      case 8:   // refresher pulling
      case 9:   // will refresh
      case 10:  // refresh
      case 11:  // restore
      case 12:  // abort
      case 13: { // status change
        const kind = Number(event.kind);
        const names = ['pulling', 'willRefresh', 'refresh', 'restore',
                       'abort', 'status'];
        const maps = [refresherPullingCallbacks, refresherWillRefreshCallbacks,
                      refresherRefreshCallbacks, refresherRestoreCallbacks,
                      refresherAbortCallbacks, refresherStatusCallbacks];
        const type = names[kind - 8];
        refresherEvents.push({ type, nodeId: tag, value, extra });
        const callback = maps[kind - 8].get(tag);
        if (callback) {
          try {
            if (type === 'status') callback(value, extra);
            else callback(value);
          } catch (error) {
            if (typeof native.log === 'function') {
              native.log(`refresher ${type} callback failed: ${error && error.message}`);
            }
          }
        }
        return;
      }
      default:
        return;
    }
  }


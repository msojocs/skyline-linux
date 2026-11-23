  function rectPayload(rect) {
    if (!rect) return { top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 };
    return {
      top: Number(rect.top) || 0,
      left: Number(rect.left) || 0,
      right: Number(rect.right) || 0,
      bottom: Number(rect.bottom) || 0,
      width: (Number(rect.right) || 0) - (Number(rect.left) || 0),
      height: (Number(rect.bottom) || 0) - (Number(rect.top) || 0),
    };
  }

  // JSON channel for intersection / selection / navigate-back events.
  function dispatchNativeJsonEvent(payload) {
    let event = null;
    try {
      event = JSON.parse(payload);
    } catch (_error) {
      return;
    }
    if (!event || typeof event.type !== 'string') return;
    const tag = event.nodeId;
    if (event.type === 'intersection') {
      intersectionEvents.push(event);
      const element = bridgeElements.get(tag);
      const listener = element && element._intersection;
      if (!listener || listener.uniqueId !== event.uniqueId) return;
      const bounds = rectPayload(event.boundingClientRect);
      const entry = {
        boundingClientRect: bounds,
        intersectionRect: rectPayload(event.intersectionRect),
        rootBounds: rectPayload(event.rootBounds),
        intersectionRatio: Number(event.intersectionRatio) || 0,
        isIntersecting: !!event.isIntersecting,
        target: element,
        time: Date.now(),
      };
      try {
        listener.callback(entry, undefined);
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`intersection callback failed: ${error && error.message}`);
        }
      }
      return;
    }
    if (event.type === 'selection') {
      selectionEvents.push(event);
      const element = bridgeElements.get(tag);
      if (element) {
        dispatchMirrorEvent(element, 'selectionchange', {
          selectedString: event.selectedString || '',
          boundingSelectionRect: {
            left: Number(event.left) || 0,
            top: Number(event.top) || 0,
            right: Number(event.right) || 0,
            bottom: Number(event.bottom) || 0,
            width: (Number(event.right) || 0) - (Number(event.left) || 0),
            height: (Number(event.bottom) || 0) - (Number(event.top) || 0),
          },
        });
      }
      return;
    }
    if (event.type === 'navigateBack') {
      navigateBackIntercepts.push(event);
      // The DevTools appservice listens for `onNavigateBackIntercept`; replay
      // it into the page world when the bridge exposes __triggerOnEvent.
      try {
        const win = typeof window !== 'undefined' ? window : globalThis;
        const bridge = win && win.WeixinJSBridge;
        if (bridge && typeof bridge.__triggerOnEvent === 'function') {
          bridge.__triggerOnEvent('onNavigateBackIntercept', event, 0);
        }
      } catch (error) {
        if (typeof native.log === 'function') {
          native.log(`navigateBack intercept dispatch failed: ${error && error.message}`);
        }
      }
      return;
    }
    // A custom route popped itself from a worklet (`didPop`) or the Flutter
    // edge-swipe gesture completed.  The DevTools only knows about
    // `wx.navigateBack`, so notify the shell so the simulator route stack
    // follows the renderer; the runtime has already popped its page stack and
    // the JS mirror must follow it for hit testing.
    if (event.type === 'navigateBackByGesture') {
      const windowId = Number(event.windowId) || 0;
      const stack = attachedRootStack.get(windowId);
      if (stack && stack.length > 1) {
        stack.pop();
        attachedRoots.set(windowId, stack[stack.length - 1]);
      }
      const callback = shellCallbacks.setNavigateBackCallback;
      if (typeof callback === 'function') {
        try {
          callback({ window_id: windowId, page_id: Number(event.pageId) || 0 });
        } catch (error) {
          if (typeof native.log === 'function') {
            native.log(`navigateBackByGesture dispatch failed: ${error && error.message}`);
          }
        }
      }
      return;
    }
    if (event.type === 'navigateBackDoneByGesture') {
      const callback = shellCallbacks.setNavigateBackDoneCallback;
      if (typeof callback === 'function') {
        try {
          callback({
            window_id: Number(event.windowId) || 0,
            page_id: Number(event.pageId) || 0,
          });
        } catch (error) {
          if (typeof native.log === 'function') {
            native.log(`navigateBackDoneByGesture dispatch failed: ${error && error.message}`);
          }
        }
      }
      return;
    }
    // Custom routes report their Flutter animation controller through
    // HandlePageRoute; the route builder registered a worklet handler named
    // `onAnimationChange-<viewTag>` (and a `status-` variant).
    if (event.type === 'pageRoute') {
      const name = typeof event.name === 'string' ? event.name : '';
      const lookup = name.startsWith('status-') ? name.slice(7) : name;
      const worklet = workletEventHandlers.get(lookup);
      if (worklet) {
        try {
          worklet.call(worklet._closure || undefined, {
            value: Number(event.value) || 0,
            status: Number(event.status) || 0,
          });
        } catch (error) {
          if (typeof native.log === 'function') {
            native.log(`page route worklet failed: ${error && error.message}`);
          }
        }
      }
      return;
    }
    // `<share-element>` Hero flight frame: native HandleHeroOnFrame reports
    // the begin/end/current rects and progress while the route transition
    // animates.  The component registered its `worklet:onframe` handler as
    // `onFrame-<viewTag>`.
    if (event.type === 'heroFrame') {
      heroFrameEvents.push(event);
      if (heroFrameEvents.length > 240) heroFrameEvents.shift();
      const worklet = workletEventHandlers.get(String(event.hash || ''));
      if (worklet) {
        try {
          worklet.call(worklet._closure || undefined, {
            type: 'frame',
            direction: Number(event.direction) || 0,
            progress: Number(event.progress) || 0,
            begin: rectPayload(event.begin),
            end: rectPayload(event.end),
            current: rectPayload(event.current),
          });
        } catch (error) {
          if (typeof native.log === 'function') {
            native.log(`hero frame worklet failed: ${error && error.message}`);
          }
        }
      }
      return;
    }
    if (event.type === 'appSnapshot' || event.type === 'nodeSnapshot') {
      snapshotEvents.push(event);
      return;
    }
    if (event.type === 'accessibilityFeatures') {
      accessibilityEvents.push(event);
      return;
    }
    if (event.type === 'userGesture') {
      userGestureEvents.push(event);
      return;
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


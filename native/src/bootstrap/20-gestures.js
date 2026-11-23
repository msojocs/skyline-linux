  // -- gesture handlers -------------------------------------------------------
  // `<pan-gesture-handler>` / `<tap-gesture-handler>` / `<*-drag-gesture-handler>`
  // are implemented by SkylineJS's `GestureHandlerModule`.  The engine's native
  // recognizers are not ported, so recognise single-pointer drags/taps against
  // the mirrored node tree and deliver them to the worklet the component
  // registered as `onGestureHandlerEvent-<handlerTag>`.
  //
  // Custom routes rely on this: card-transition's detail page drags its pan
  // handler to the right, then calls the route context's `didPop()`.
  const gestureHandlers = new Map();      // handlerTag -> entry
  const gestureHandlersByNode = new Map(); // node viewTag -> entry
  let activeGesture = null;
  let longPressTimer = null;
  let lastTap = null;

  const GESTURE_TAP = 0;
  const GESTURE_DOUBLE_TAP = 1;
  const GESTURE_LONG_PRESS = 2;
  const GESTURE_PAN = 3;
  const GESTURE_SCALE = 4;
  const GESTURE_FORCE_PRESS = 5;
  const GESTURE_VERTICAL_DRAG = 6;
  const GESTURE_HORIZONTAL_DRAG = 7;
  const GESTURE_STATE_POSSIBLE = 0;
  const GESTURE_STATE_BEGIN = 1;
  const GESTURE_STATE_ACTIVE = 2;
  const GESTURE_STATE_END = 3;
  const GESTURE_STATE_CANCELLED = 4;
  const GESTURE_SLOP = 6;

  function gestureHitElement(windowId, x, y) {
    const root = attachedRoots.get(windowId);
    try {
      return root ? hitTestElement(windowId, root, x, y) : null;
    } catch (_error) {
      return null;
    }
  }

  function gestureHandlerAt(windowId, x, y) {
    const hit = gestureHitElement(windowId, x, y);
    for (let node = hit; node; node = node.parent) {
      const entry = gestureHandlersByNode.get(node.viewTag);
      if (entry) return entry;
    }
    // Zero-area wrappers (auto-sized absolute containers) are not hit, so fall
    // back to the registered handler boxes and prefer the current page.
    const root = attachedRoots.get(windowId);
    let best = null;
    let bestArea = Infinity;
    for (const entry of gestureHandlers.values()) {
      const node = entry.node;
      if (!node || !node.viewTag) continue;
      let current = false;
      for (let cursor = node; cursor; cursor = cursor.parent) {
        if (cursor === root) { current = true; break; }
      }
      if (root && !current) continue;
      const rect = nodeRect(windowId, node.viewTag);
      if (!rect || rect.width <= 0 || rect.height <= 0) continue;
      if (x < rect.left || x > rect.left + rect.width ||
          y < rect.top || y > rect.top + rect.height) continue;
      const area = rect.width * rect.height;
      if (area < bestArea) { best = entry; bestArea = area; }
    }
    return best;
  }

  // A drag that starts inside a native swiper belongs to the swiper's own
  // paging recognizer.  When the gesture handler is registered on an ancestor
  // (card-transition wraps the whole page in a `<pan-gesture-handler>`), the
  // swiper wins the negotiation for drags along its paging axis; otherwise the
  // custom-route back animation also runs while the slideshow pages.  Handlers
  // registered inside a swiper keep receiving their events.
  function gestureSwiperAxis(windowId, x, y, entry) {
    if (!entry || !entry.node) return null;
    let swiper = null;
    for (let node = gestureHitElement(windowId, x, y); node;
         node = node.parent) {
      if (node.name === 'swiper' || node.tag === 'swiper') {
        swiper = node;
        break;
      }
    }
    if (!swiper) return null;
    let wrapsSwiper = false;
    for (let node = swiper; node; node = node.parent) {
      if (node === entry.node) { wrapsSwiper = true; break; }
    }
    if (!wrapsSwiper) return null;
    const attrs = swiper.attrs || {};
    const vertical = attrs.vertical === '1' || attrs.vertical === 'true' ||
      attrs.vertical === true;
    return vertical ? 'vertical' : 'horizontal';
  }

  function gesturePayload(entry, state, x, y, deltaX, deltaY, vx, vy) {
    const event = {
      handlerTag: entry.handlerTag,
      gestureType: entry.gestureType,
      state,
      absoluteX: x,
      absoluteY: y,
      x,
      y,
      deltaX,
      deltaY,
      translationX: x - entry.startX,
      translationY: y - entry.startY,
      velocityX: vx,
      velocityY: vy,
      pointerCount: 1,
      timeStamp: Date.now(),
    };
    if (entry.gestureType === GESTURE_SCALE) {
      event.focalX = x;
      event.focalY = y;
      event.focalDeltaX = deltaX;
      event.focalDeltaY = deltaY;
      event.scale = 1;
      event.horizontalScale = 1;
      event.verticalScale = 1;
      event.rotation = 0;
    }
    return event;
  }

  function gestureWorklet(name, entry) {
    return workletEventHandlers.get(`${name}-${entry.handlerTag}`);
  }

  function emitGesture(entry, state, x, y, dx, dy, vx, vy) {
    const worklet = gestureWorklet('onGestureHandlerEvent', entry);
    if (typeof worklet !== 'function') return;
    try {
      worklet.call(worklet._closure || undefined,
                   gesturePayload(entry, state, x, y, dx, dy, vx, vy));
    } catch (error) {
      if (typeof native.log === 'function') {
        native.log(`gesture worklet failed: ${error && error.message}`);
      }
    }
  }

  function gestureShouldRespond(entry, x, y, dx, dy) {
    const worklet = gestureWorklet('shouldResponseOnMove', entry);
    if (typeof worklet !== 'function') return true;
    try {
      return !!worklet.call(worklet._closure || undefined,
                            gesturePayload(entry, GESTURE_STATE_ACTIVE,
                                           x, y, dx, dy, 0, 0));
    } catch (_error) {
      return true;
    }
  }

  function gestureCancelLongPress() {
    if (longPressTimer !== null) {
      clearTimeout(longPressTimer);
      longPressTimer = null;
    }
  }

  function gestureTouchStart(windowId, x, y) {
    const entry = gestureHandlerAt(windowId, x, y);
    if (!entry) return;
    gestureCancelLongPress();
    activeGesture = entry;
    entry.startX = x;
    entry.startY = y;
    entry.startTime = Date.now();
    entry.lastX = x;
    entry.lastY = y;
    entry.lastTime = Date.now();
    entry.lastVX = 0;
    entry.lastVY = 0;
    entry.moved = false;
    entry.active = false;
    entry.cancelled = false;
    entry.swiperAxis = gestureSwiperAxis(windowId, x, y, entry);
    emitGesture(entry, GESTURE_STATE_POSSIBLE, x, y, 0, 0, 0, 0);
    if (entry.gestureType === GESTURE_LONG_PRESS) {
      longPressTimer = setTimeout(() => {
        longPressTimer = null;
        if (activeGesture !== entry || entry.moved) return;
        emitGesture(entry, GESTURE_STATE_BEGIN, entry.lastX, entry.lastY,
                    0, 0, 0, 0);
      }, 500);
    }
  }

  function gestureTouchMove(windowId, x, y) {
    const entry = activeGesture;
    if (!entry) return;
    const now = Date.now();
    const dx = x - entry.lastX;
    const dy = y - entry.lastY;
    const dt = Math.max(1, now - entry.lastTime);
    const vx = (dx / dt) * 1000;
    const vy = (dy / dt) * 1000;
    entry.lastVX = vx;
    entry.lastVY = vy;
    entry.lastX = x;
    entry.lastY = y;
    entry.lastTime = now;
    const totalX = x - entry.startX;
    const totalY = y - entry.startY;
    if (Math.abs(totalX) > 10 || Math.abs(totalY) > 10) entry.moved = true;

    if (!entry.active) {
      const distance = Math.sqrt(totalX * totalX + totalY * totalY);
      if (distance < GESTURE_SLOP) return;
      if (entry.gestureType === GESTURE_VERTICAL_DRAG &&
          Math.abs(totalY) <= Math.abs(totalX)) return;
      if (entry.gestureType === GESTURE_HORIZONTAL_DRAG &&
          Math.abs(totalX) <= Math.abs(totalY)) return;
      // The swiper claims drags along its paging axis; do not start an outer
      // pan/back gesture for them.  A perpendicular drag still bubbles.
      if (entry.swiperAxis) {
        const horizontalDrag = Math.abs(totalX) >= Math.abs(totalY);
        if ((entry.swiperAxis === 'horizontal') === horizontalDrag) {
          entry.cancelled = true;
          return;
        }
        entry.swiperAxis = null;
      }
      gestureCancelLongPress();
      if (!gestureShouldRespond(entry, x, y, totalX, totalY)) return;
      const accept = gestureWorklet('shouldAcceptGesture', entry);
      if (typeof accept === 'function') {
        try {
          if (!accept.call(accept._closure || undefined)) {
            entry.cancelled = true;
            return;
          }
        } catch (_error) {}
      }
      entry.active = true;
      emitGesture(entry, GESTURE_STATE_BEGIN, x, y, dx, dy, vx, vy);
    }
    if (entry.cancelled) return;
    emitGesture(entry, GESTURE_STATE_ACTIVE, x, y, dx, dy, vx, vy);
  }

  function gestureTouchEnd(windowId, x, y, cancelled) {
    const entry = activeGesture;
    gestureCancelLongPress();
    if (!entry) return;
    activeGesture = null;
    entry.swiperAxis = null;
    if (entry.gestureType === GESTURE_TAP ||
        entry.gestureType === GESTURE_DOUBLE_TAP) {
      if (!entry.moved && !cancelled) {
        if (entry.gestureType === GESTURE_DOUBLE_TAP) {
          const now = Date.now();
          const previous = lastTap;
          lastTap = { entry, time: now, x, y };
          if (!previous || previous.entry !== entry ||
              now - previous.time > 320 ||
              Math.abs(previous.x - x) > 30 || Math.abs(previous.y - y) > 30) {
            return;
          }
          lastTap = null;
        }
        emitGesture(entry, GESTURE_STATE_BEGIN, x, y, 0, 0, 0, 0);
      }
      return;
    }
    if (entry.active && !entry.cancelled) {
      emitGesture(entry,
                  cancelled ? GESTURE_STATE_CANCELLED : GESTURE_STATE_END,
                  x, y, 0, 0, entry.lastVX, entry.lastVY);
    } else if (!entry.cancelled) {
      emitGesture(entry, GESTURE_STATE_CANCELLED, x, y, 0, 0, 0, 0);
    }
  }

  function dispatchGestureTouch(windowId, kind, x, y) {
    if (kind === 'start') {
      gestureTouchStart(windowId, x, y);
    } else if (kind === 'move') {
      gestureTouchMove(windowId, x, y);
    } else if (kind === 'end' || kind === 'cancel') {
      gestureTouchEnd(windowId, x, y, kind === 'cancel');
    }
  }

  function createGestureHandlerModule() {
    const module = {
      attachGestureHandler(handlerTag, gestureType, config, hostNode) {
        const tag = Number(handlerTag) || 0;
        if (!tag) return;
        const entry = {
          handlerTag: tag,
          gestureType: Number(gestureType) || 0,
          config: config || {},
          node: hostNode || null,
          active: false,
          startX: 0,
          startY: 0,
          lastX: 0,
          lastY: 0,
          swiperAxis: null,
        };
        gestureHandlers.set(tag, entry);
        if (hostNode && hostNode.viewTag) {
          gestureHandlersByNode.set(hostNode.viewTag, entry);
        }
      },
      updateGestureHandler(handlerTag, hostNode, config) {
        const entry = gestureHandlers.get(Number(handlerTag) || 0);
        if (!entry) return;
        if (hostNode && hostNode.viewTag) {
          entry.node = hostNode;
          gestureHandlersByNode.set(hostNode.viewTag, entry);
        }
        if (config) entry.config = config;
      },
      dropGestureHandler(handlerTag) {
        const tag = Number(handlerTag) || 0;
        const entry = gestureHandlers.get(tag);
        if (entry && entry.node && entry.node.viewTag) {
          gestureHandlersByNode.delete(entry.node.viewTag);
        }
        gestureHandlers.delete(tag);
        if (activeGesture === entry) activeGesture = null;
      },
      // `native-view` handlers (e.g. the previewer's swiper) negotiate with the
      // Flutter scrollable/swiper; only the worklet registration is needed.
      // The SDK's gesture-handler component reaches this through
      // `attachNativeViewGestureHandler` (proxy nodes) while
      // `proxyScrollViewGestureHandler` is the older scroll-view spelling.
      attachNativeViewGestureHandler(handlerTag, gestureType, config,
                                     hostNode) {
        this.attachGestureHandler(handlerTag, gestureType, config, hostNode);
      },
      proxyScrollViewGestureHandler(handlerTag, gestureType, config, hostNode) {
        this.attachGestureHandler(handlerTag, gestureType, config, hostNode);
      },
    };
    return module;
  }

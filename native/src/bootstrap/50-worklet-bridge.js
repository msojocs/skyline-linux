  // -- worklet mappers (applyAnimatedStyle / derived) ------------------------
  // The SDK's `applyAnimatedStyle` and `wx.worklet.derived` register a mapper
  // with `SkylineGlobal.workletModule.startMapper(selector, worklet, inputs,
  // outputs, config)`.  The official renderer runs that worklet whenever one
  // of its input shared values changes; the worklet applies the style through
  // `workletUIModule.updateNodeStyle`.  Without this emulation nothing drives
  // worklet animations (a custom-route drag stayed frozen while the finger
  // moved and only the release animation ran).
  const workletMappers = new Map();
  let nextWorkletMapperId = 1;
  let workletMapperScheduled = false;

  // Collects worklet-mutable-like inputs.  The closure values are arbitrary
  // objects (the component instance, plain objects, arrays); recurse with a
  // budget and skip the global object so a captured `globalThis` cannot walk
  // the entire realm.
  function collectWorkletInputs(value, out, seen, depth) {
    if (value === null || typeof value !== 'object') return;
    if (typeof value.addListener === 'function' && 'value' in value) {
      if (!seen.has(value)) {
        seen.add(value);
        out.push(value);
      }
      return;
    }
    if (depth > 6 || seen.size > 500) return;
    if (seen.has(value)) return;
    if (value === globalThis ||
        (typeof window !== 'undefined' && value === window)) {
      return;
    }
    seen.add(value);
    if (Array.isArray(value)) {
      for (const item of value) {
        collectWorkletInputs(item, out, seen, depth + 1);
      }
      return;
    }
    let keys;
    try {
      keys = Object.keys(value);
    } catch (_error) {
      return;
    }
    for (const key of keys) {
      if (key === 'window' || key === 'globalThis' || key === 'global') continue;
      let child;
      try {
        child = value[key];
      } catch (_error) {
        continue;
      }
      collectWorkletInputs(child, out, seen, depth + 1);
    }
  }

  function elementIsMounted(element) {
    let root = element;
    while (root && root.parent) root = root.parent;
    if (!root) return false;
    for (const attached of attachedRoots.values()) {
      if (attached === root) return true;
    }
    for (const portals of detachedPortals.values()) {
      for (const portal of portals) {
        if (portal === root) return true;
      }
    }
    return false;
  }

  function elementMatchesCompound(element, compound) {
    if (!element || !compound) return false;
    let rest = compound;
    const tagMatch = /^[A-Za-z_][\w-]*/.exec(rest);
    if (tagMatch) {
      const tag = tagMatch[0];
      if (tag !== element.tag && tag !== element.name) return false;
      rest = rest.slice(tag.length);
    }
    while (rest.length > 0) {
      const match = /^([#.])([\w-]+)/.exec(rest);
      if (!match) return false;
      rest = rest.slice(match[0].length);
      if (match[1] === '#') {
        if (element._id !== match[2]) return false;
      } else if (!Array.isArray(element._classNames) ||
                 element._classNames.indexOf(match[2]) < 0) {
        return false;
      }
    }
    return true;
  }

  function elementMatchesSelector(element, selector) {
    const groups = String(selector).split(',');
    for (const group of groups) {
      const parts = group.replace(/\s*>\s*/g, ' ').trim().split(/\s+/);
      if (parts.length === 0 || !parts[0]) continue;
      if (!elementMatchesCompound(element, parts[parts.length - 1])) continue;
      let index = parts.length - 2;
      let ancestor = element.parent;
      while (index >= 0 && ancestor) {
        if (elementMatchesCompound(ancestor, parts[index])) index -= 1;
        ancestor = ancestor.parent;
      }
      if (index < 0) return true;
    }
    return false;
  }

  // Resolves `applyAnimatedStyle` targets.  `selector` is normally a CSS
  // selector string; components may also pass an element/backend node directly.
  // `owner` is the component backend element that registered the mapper:
  // class selectors are component-scoped (`applyAnimatedStyle('.card_wrap')`
  // must not paint every card in the list).
  function isSelfOrDescendant(element, owner) {
    for (let node = element; node; node = node.parent) {
      if (node === owner) return true;
    }
    return false;
  }

  function workletSelectorNodes(selector, owner) {
    const direct = (value) => {
      if (!value || typeof value !== 'object') return null;
      if (typeof value.setAnimatedStyle === 'function') return value;
      const backend = value.$$;
      if (backend && typeof backend.setAnimatedStyle === 'function') {
        return backend;
      }
      return null;
    };
    if (typeof selector !== 'string') {
      if (Array.isArray(selector)) {
        const nodes = [];
        for (const entry of selector) {
          for (const node of workletSelectorNodes(entry, owner)) {
            if (nodes.indexOf(node) < 0) nodes.push(node);
          }
        }
        return nodes;
      }
      const one = direct(selector);
      return one ? [one] : [];
    }
    const matches = [];
    const unmounted = [];
    for (const element of bridgeElements.values()) {
      if (!element || element.released) continue;
      if (owner && !isSelfOrDescendant(element, owner)) continue;
      if (!elementMatchesSelector(element, selector)) continue;
      if (elementIsMounted(element)) {
        matches.push(element);
      } else {
        unmounted.push(element);
      }
    }
    // Selectors created during `attached` run before the root is registered in
    // every host; fall back to the detached matches rather than never painting.
    return matches.length > 0 ? matches : unmounted;
  }

  function executeWorkletMapper(entry) {
    entry.dirty = false;
    let nodes;
    // The SDK resolves `applyAnimatedStyle` selectors itself and registers the
    // mapper on the resolved backend element with `addAnimatedStyle(mapperId)`;
    // `startMapper` only receives the window id (4) as its selector argument.
    const selectorLike = typeof entry.selector === 'string' ||
      Array.isArray(entry.selector) ||
      (entry.selector !== null && entry.selector !== undefined &&
       typeof entry.selector === 'object');
    if (selectorLike) {
      nodes = workletSelectorNodes(entry.selector, entry.owner);
      if (nodes.length === 0 && entry.owner) nodes = [entry.owner];
      if (nodes.length === 0) {
        // The selector may not be attached yet (or its route is still being
        // built); retry for about a second before giving up.
        if (entry.misses < 60) {
          entry.misses += 1;
          entry.dirty = true;
        }
        return;
      }
    } else if (entry.owner) {
      // Target the element the mapper was registered on.
      nodes = [entry.owner];
    } else {
      // `derived()` mappers have no owner and ignore the node list (they
      // assign their output shared value themselves).
      nodes = undefined;
    }
    entry.misses = 0;
    try {
      entry.worklet.call(entry.worklet._closure || undefined, undefined, nodes);
    } catch (error) {
      if (typeof native.log === 'function') {
        native.log(`worklet mapper failed: ${error && error.message}`);
      }
    }
  }

  function runWorkletMappers() {
    let pending = false;
    for (const entry of Array.from(workletMappers.values())) {
      if (!entry.dirty) continue;
      executeWorkletMapper(entry);
      if (entry.dirty) pending = true;
    }
    if (pending) scheduleWorkletMapperFrame();
  }

  function scheduleWorkletMapperFrame() {
    if (workletMapperScheduled) return;
    workletMapperScheduled = true;
    setTimeout(() => {
      workletMapperScheduled = false;
      runWorkletMappers();
    }, 16);
  }

  function stopWorkletMapper(mapperId) {
    const entry = workletMappers.get(mapperId);
    if (!entry) return;
    workletMappers.delete(mapperId);
    for (const input of entry.inputs) {
      try {
        input.removeListener(entry.listener);
      } catch (_error) {}
    }
  }

  // The miniprogram SDK's worklet code references the native-provided
  // `workletUIModule` as a bare global (`setGlobalConsole`,
  // `getBoundingClientRect`, `scrollTo`, ...).  The DevTools appservice never
  // installs it, so the first worklet run throws
  // `ReferenceError: workletUIModule is not defined` at launch.  Provide the
  // methods the SDK probes for; the widget helpers fall back to the SDK's own
  // zero-rect/no-op behaviour.
  //
  // The route-manipulation entry points are *not* no-ops: custom-route gesture
  // code (`didPop`/`startUserGesture`/...) runs as a worklet and can only reach
  // the Flutter route through the native bridge.  The native runtime looks up
  // the Dart `NavigatorPop`/`StartUserGesture`/`StopUserGesture`/
  // `UpdateAnimationController` callbacks registered in its FFI table.
  function createWorkletUIModule() {
    return {
      setGlobalConsole() {},
      getBoundingClientRect() {
        return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 };
      },
      getCurrentTime() { return Date.now(); },
      navigatorPop(windowId, pageId) {
        if (typeof native.navigatorPop === 'function') {
          native.navigatorPop(Number(windowId) || 0, Number(pageId) || 0);
        }
      },
      // `wx.worklet.timing` / `decay` drive themselves with requestFrame on the
      // UI runtime; without it a released custom-route gesture never reaches
      // its completion callback (which calls `didPop()`).
      requestFrame(windowId, frame) {
        if (typeof windowId === 'function' && typeof frame !== 'function') {
          frame = windowId;
        }
        if (typeof frame !== 'function') return 0;
        return setTimeout(() => {
          try {
            frame(Date.now());
          } catch (_error) {}
        }, 16);
      },
      cancelFrame(windowId, frameId) {
        const id = typeof windowId === 'number' && frameId === undefined
          ? windowId
          : frameId;
        if (id) clearTimeout(id);
      },
      scrollTo() {},
      snapshotChangeMode() {},
      startUserGesture(windowId) {
        if (typeof native.startUserGesture === 'function') {
          native.startUserGesture(Number(windowId) || 0);
        }
      },
      stopUserGesture(windowId) {
        if (typeof native.stopUserGesture === 'function') {
          native.stopUserGesture(Number(windowId) || 0);
        }
      },
      updateAnimationController(windowId, pageId, value) {
        if (typeof native.updateAnimationController === 'function') {
          native.updateAnimationController(Number(windowId) || 0,
                                           Number(pageId) || 0,
                                           Number(value) || 0);
        }
      },
      updateNodeStyle(node, style) {
        let element = null;
        if (node && typeof node.setAnimatedStyle === 'function') {
          element = node;
        } else if (typeof node === 'number') {
          element = bridgeElements.get(node) || null;
        } else if (node && node.$$ &&
                   typeof node.$$.setAnimatedStyle === 'function') {
          element = node.$$;
        }
        if (element) {
          element.setAnimatedStyle(style == null ? '' : String(style));
        }
      },
    };
  }

  function installWorkletUIModule(win) {
    if (!win) return;
    try {
      if (!win.workletUIModule) win.workletUIModule = createWorkletUIModule();
      const module = win.workletUIModule;
      if (!module) return;
      // The reanimated worklet sources emitted into assets/app.js call bare
      // `_startUserGesture`/`_navigatorPop`/`_updateAnimationController`
      // globals instead of `workletUIModule.*`; alias them for that realm.
      if (typeof win._startUserGesture !== 'function') {
        win._startUserGesture = function (windowId) {
          return module.startUserGesture(windowId);
        };
      }
      if (typeof win._stopUserGesture !== 'function') {
        win._stopUserGesture = function (windowId) {
          return module.stopUserGesture(windowId);
        };
      }
      if (typeof win._navigatorPop !== 'function') {
        win._navigatorPop = function (windowId, pageId) {
          return module.navigatorPop(windowId, pageId);
        };
      }
      if (typeof win._updateAnimationController !== 'function') {
        win._updateAnimationController = function (windowId, pageId, value) {
          return module.updateAnimationController(windowId, pageId, value);
        };
      }
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


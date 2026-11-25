  class BridgePageContext {
    constructor(id, windowId, featureFlags) {
      this.id = id;
      this.windowId = windowId;
      // Renderer options (defaultContentBox/defaultBlockLayout/...) arrive as
      // the SDK's third constructor argument; forward them with every batch.
      this.featureFlags =
        featureFlags && typeof featureFlags === 'object' ? featureFlags : null;
      // Each renderer context (page, app bar, tab bar) owns its operations.
      // Sharing one per-window buffer mixed the page and app-bar trees in the
      // same batch, so the native replay could not tell which root a batch
      // belonged to and materialised the app bar as an extra page route.
      this.ops = [];
      this.sent = false;
      this.flushing = false;
      this.flushTimer = null;
      this.rootNode = null;
      this.rendered = false;
      this.isAppBar = false;
      this.frameworkType = 'glass-easel';
      liveContexts.add(this);
      // Selector-query viewport rect reads these (see WASubContext's
      // SelectorQuery for `selectViewport()`).
      const size = windowSize(windowId);
      this.clientWidth = size ? size.width : 0;
      this.clientHeight = size ? size.height : 0;
      this.ops.push({ op: 'createContext', id, windowId,
                      flags: this.featureFlags });
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
    // `render(callback)` is the setData completion path: the appservice SDK
    // calls `triggerRender` -> `backendContext.render(cb)` after applying data
    // updates and relies on the callback to run `setData(data, callback)`.
    render(callback) {
      this.flush();
      if (typeof callback === 'function') {
        setTimeout(() => callback(null), 0);
      }
    }
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
      // `backfillHeroAttributes` must run before serialisation so the
      // share-element props reach native in the same batch as the page
      // `attach`; the runtime materialises the page after the whole batch, so
      // the Hero then gets the real tag/gesture/shuttle values.
      backfillHeroAttributes();
      if (this.flushing || this.ops.length === 0) return;
      // The native replay keeps one node table per window.  Tag every batch
      // with the originating context so updates rebuild the right root (the
      // app-bar context is an overlay, not a page route).
      if (!this.ops[0] || this.ops[0].op !== 'createContext') {
        this.ops.unshift({ op: 'createContext', id: this.id,
                           windowId: this.windowId,
                           flags: this.featureFlags });
      }
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

  // Skyline app-bar contexts (`app.json#appBar`) clone a page context but the
  // SDK attaches them through `SkylineGlobal.runtime.attachAppBar` and expects
  // the runtime to install them as a window overlay (Dart `CreateAppBar`),
  // not as another page route.  The SDK's base `attach()` still runs, so tag
  // the attach so the native replay can tell them apart.
  class BridgeAppBarContext extends BridgePageContext {
    constructor(id, windowId, featureFlags) {
      super(id, windowId, featureFlags);
      this.isAppBar = true;
    }
    attach(root, host) {
      this.ops.push({ op: 'attachAppBar', root: bridgeId(root), host: bridgeId(host) });
      if (root && typeof root === 'object') {
        appBarRoots.set(this.windowId, root);
      }
      if (this.flushTimer === null) {
        this.flushTimer = setTimeout(() => { this.flushTimer = null; this.flush(); }, 2000);
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
  // App-bar overlay roots per window.  They paint above the page stack, so the
  // hit test must visit them first and fall through to the page when the
  // collapsed bar does not cover the point.
  const appBarRoots = new Map();
  // Attached roots in page-stack order (skyline keeps covered pages alive
  // while navigateTo pushes a new route).  navigateBack restores the previous
  // root here as well so the JS hit test follows the visible page.
  const attachedRootStack = new Map();
  const detachedPortals = new Map();
  const lastTouchPoint = new Map();
  // Debug handle for CDP inspection of the mirrored element tree.
  globalThis.__skylineDebug = {
    attachedRoots, appBarRoots, detachedPortals, lastTouchPoint,
    touchCallbacks: runtimeTouchCallbacks,
    scrollOffsets,
    workletEventHandlers,
    workletMappers,
    resolveWorkletNodes: workletSelectorNodes,    scrollEventCallbacks,
    swiperScrollEventCallbacks,
    scrollStartEventCallbacks,
    scrollEndEventCallbacks,
    pickerEvents,
    inputEvents,
    intersectionEvents,
    selectionEvents,
    refresherEvents,
    navigateBackIntercepts,
    accessibilityEvents,
    snapshotEvents,
    userGestureEvents,
    heroFrameEvents,
    heroElements,
    bridgeElements,
    gestureHandlers,
    rect: nodeRect,
    hit: hitTestElement,
    selectionSetRange(windowId, selectionId, start, startOffset, end, endOffset) {
      if (typeof native.selectionSetRange === 'function') {
        return native.selectionSetRange(windowId, selectionId, start,
                                        startOffset, end, endOffset);
      }
      return undefined;
    },
    refresherTriggered(windowId, nodeId, open, twoLevel) {
      if (typeof native.refresherTriggered === 'function') {
        return native.refresherTriggered(windowId, nodeId, !!open, !!twoLevel);
      }
      return undefined;
    },
    setNavigateBackInterception(windowId, pageId, enabled) {
      if (typeof native.setNavigateBackInterception === 'function') {
        return native.setNavigateBackInterception(windowId, pageId, !!enabled);
      }
      return undefined;
    },
    swiperTo(windowId, nodeId, page, animated) {
      if (typeof native.swiperTo === 'function') {
        return native.swiperTo(windowId, nodeId, page, animated !== false);
      }
      return undefined;
    },
    pickerColumnSetCurrent(windowId, nodeId, current) {
      if (typeof native.pickerColumnSetCurrent === 'function') {
        return native.pickerColumnSetCurrent(windowId, nodeId, current);
      }
      return undefined;
    },
    inputFocus(windowId, nodeId, focus) {
      if (typeof native.inputFocus === 'function') {
        return native.inputFocus(windowId, nodeId, !!focus);
      }
      return undefined;
    },
    scrollTo(windowId, nodeId, top) {
      if (typeof native.log === 'function') {
        native.log(`scroll-into-view node=${nodeId} top=${top}`);
      }
      if (typeof native.scrollNodeTo === 'function') {
        native.scrollNodeTo(windowId, nodeId, top);
      }
    },
    // -- D10: accessibility + DevTools automation ---------------------------
    setAccessibility(windowId, enabled) {
      if (typeof native.setAccessibility === 'function') {
        return native.setAccessibility(windowId, !!enabled);
      }
      return undefined;
    },
    accessibilityState() {
      return typeof native.accessibilityState === 'function'
        ? native.accessibilityState()
        : 0;
    },
    dumpElementTree(windowId) {
      if (typeof native.dumpElementTree === 'function') {
        return native.dumpElementTree(windowId);
      }
      return undefined;
    },
    dumpRenderTree(windowId) {
      if (typeof native.dumpRenderTree === 'function') {
        return native.dumpRenderTree(windowId);
      }
      return undefined;
    },
    collectGarbage(windowId) {
      if (typeof native.collectGarbage === 'function') {
        return native.collectGarbage(windowId);
      }
      return undefined;
    },
    sendMessageToRunner(windowId, message) {
      if (typeof native.sendMessageToRunner === 'function') {
        return native.sendMessageToRunner(windowId, String(message));
      }
      return undefined;
    },
    takeAppSnapshot(windowId, requestId, scale, compressed) {
      if (typeof native.takeAppSnapshot === 'function') {
        return native.takeAppSnapshot(windowId, requestId,
          typeof scale === 'number' ? scale : 1,
          compressed !== false);
      }
      return undefined;
    },
    snapshotNode(windowId, nodeId, format, requestId) {
      if (typeof native.snapshotNode === 'function') {
        return native.snapshotNode(windowId, nodeId,
          typeof format === 'number' ? format : 4,
          typeof requestId === 'number' ? requestId : nodeId);
      }
      return undefined;
    },
    setUserGesture(windowId, active) {
      if (typeof native.setUserGesture === 'function') {
        return native.setUserGesture(windowId, !!active);
      }
      return undefined;
    },
    relaunchApp(windowId, openType) {
      if (typeof native.relaunchApp === 'function') {
        return native.relaunchApp(windowId, openType || 0);
      }
      return undefined;
    },
    shutdownApplication(windowId) {
      if (typeof native.shutdownApplication === 'function') {
        return native.shutdownApplication(windowId);
      }
      return undefined;
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
      // CSS pointer-events:none removes the element and its subtree from hit
      // testing (the native YGNode receives PointerEvents.none as well).
      if (rect.pointerEvents) continue;
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
    // The app bar paints above the page stack; only fall through when none of
    // its elements (the collapsed player bar) cover the touch point.
    const appBar = appBarRoots.get(windowId);
    if (appBar) roots.push(appBar);
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


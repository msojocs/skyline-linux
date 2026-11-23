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
      this._baseStyle = '';
      this._animatedStyle = '';
      this._classNames = [];
      this.viewTag = ++nextBridgeTag;
      this.ops.push({ op: 'createElement', id: this.viewTag, tag, name });
      bridgeElements.set(this.viewTag, this);
      if (tag === 'share-element' || name === 'share-element') {
        heroElements.set(this.viewTag, this);
      }
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
      this._baseStyle = String(value);
      this._pushCombinedStyle();
    }
    // Worklet-driven styles (`applyAnimatedStyle`) live on top of the element's
    // inline style: composing keeps the WXML declarations intact while the
    // mapper repaints its animated properties every frame.
    setAnimatedStyle(value) {
      const next = value == null ? '' : String(value);
      if (next === this._animatedStyle) return;
      this._animatedStyle = next;
      this._pushCombinedStyle();
    }
    _pushCombinedStyle() {
      let combined = this._baseStyle;
      if (this._animatedStyle) {
        combined = combined ? `${combined};${this._animatedStyle}`
                            : this._animatedStyle;
      }
      if (combined === this._style) return;
      this._style = combined;
      this.ops.push({ op: 'setStyle', id: this.viewTag, value: this._style });
    }
    addClass(prefix, scope) {
      const name = String(prefix == null ? '' : prefix);
      if (name && this._classNames.indexOf(name) < 0) {
        this._classNames.push(name);
      }
      this.ops.push({ op: 'addClass', id: this.viewTag, prefix, name: scope });
    }
    removeClass(prefix, scope) {
      const name = String(prefix == null ? '' : prefix);
      const index = this._classNames.indexOf(name);
      if (index >= 0) this._classNames.splice(index, 1);
      this.ops.push({ op: 'removeClass', id: this.viewTag, prefix, name: scope });
    }
    setClass(names) {
      this._classNames = String(names == null ? '' : names)
        .split(/\s+/)
        .filter(Boolean);
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
      const mapperId = typeof style === 'number'
        ? style
        : (style && typeof style === 'object' ? style.mapperId : undefined);
      if (typeof mapperId === 'number') {
        // The SDK registers the mapper on the component's backend element; use
        // it to keep class selectors scoped to that component's subtree.
        const entry = workletMappers.get(mapperId);
        if (entry) entry.owner = this;
      }
      this.ops.push({
        op: 'addAnimatedStyle',
        id: this.viewTag,
        mapperId,
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
      const state = scrollOffsets.get(this.viewTag);
      const offset = {
        scrollLeft: state ? state.scrollLeft : 0,
        scrollTop: state ? state.scrollTop : 0,
        scrollWidth: state ? state.scrollWidth : (rect ? rect.width : 0),
        scrollHeight: state ? state.scrollHeight : (rect ? rect.height : 0),
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
      clone._baseStyle = this._baseStyle;
      clone._animatedStyle = this._animatedStyle;
      clone._style = this._style;
      clone._classNames = this._classNames.slice();
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
    get classList() { return this._classNames; }
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
    onScrollEvent(callback) {
      const isSwiper = this.name === 'swiper';
      const map = isSwiper ? swiperScrollEventCallbacks : scrollEventCallbacks;
      if (typeof callback === 'function') {
        map.set(this.viewTag, callback);
      } else {
        map.delete(this.viewTag);
      }
    }
    onLoadEvent(callback) { this.loadEventCallback = callback; }
    onErrorEvent(callback) { this.errorEventCallback = callback; }
    onKeyboardValueChange(callback) { this.keyboardValueChangeCallback = callback; }
    onKeyboardShow(callback) { this.keyboardShowCallback = callback; }
    onKeyboardHeightChange(callback) { this.keyboardHeightChangeCallback = callback; }
    onKeyboardConfirm(callback) { this.keyboardConfirmCallback = callback; }
    onKeyboardComplete(callback) { this.keyboardCompleteCallback = callback; }
    // The builtin <input>/<textarea> register every keyboard channel in
    // `created`; the native widget event channel only drives value changes for
    // now, but the registrations must exist or component creation throws.
    onKeyboardSelectionChange(callback) {
      this.keyboardSelectionChangeCallback = callback;
    }
    onKeyboardCompositionStart(callback) {
      this.keyboardCompositionStartCallback = callback;
    }
    onKeyboardCompositionEnd(callback) {
      this.keyboardCompositionEndCallback = callback;
    }
    onKeyboardCompositionUpdate(callback) {
      this.keyboardCompositionUpdateCallback = callback;
    }
    onKeyboardLineChange(callback) {
      this.keyboardLineChangeCallback = callback;
    }
    onSetValueCompleted(callback) {
      this.setValueCompletedCallback = callback;
    }
    focus() {
      if (typeof native.inputFocus === 'function') {
        native.inputFocus(this.windowId, this.viewTag, true);
      }
    }
    blur() {
      if (typeof native.inputFocus === 'function') {
        native.inputFocus(this.windowId, this.viewTag, false);
      }
    }
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
    // Appservice SDK selector result path: it calls
    // `node.createIntersectionObserver(root, margins, thresholds, cb)` and
    // expects `cb(entries)` with one entry.
    createIntersectionObserver(root, margins, thresholds, callback) {
      const self = this;
      self.setIntersectionListener(
        { root: root || null, rootMargin: margins || [0, 0, 0, 0],
          thresholds: thresholds || [0] },
        (entry) => {
          if (typeof callback === 'function') callback([entry]);
        });
      return {
        disconnect() { self.setIntersectionListener({}, null); },
      };
    }
    getBeforePseudoNode() { return null; }
    getAfterPseudoNode() { return null; }
    updateStyle(value) { this.setStyle(value); }
    appendStyle(value) { this.setStyle(`${this._baseStyle}${value}`); }
    clearAnimatedStyle(mapperId) {
      const id = typeof mapperId === 'number'
        ? mapperId
        : (mapperId && typeof mapperId === 'object' ? mapperId.mapperId : NaN);
      if (typeof id === 'number' && !Number.isNaN(id)) {
        stopWorkletMapper(id);
      }
    }
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
    requestRefresherRefresh() {
      if (typeof native.refresherTriggered === 'function') {
        native.refresherTriggered(this.windowId, this.viewTag, true, false);
      }
    }
    requestRefresherTwoLevel() {
      if (typeof native.refresherTriggered === 'function') {
        native.refresherTriggered(this.windowId, this.viewTag, true, true);
      }
    }
    getScrollPosition() { return 0; }
    onScrollStartEvent(callback) {
      if (typeof callback === 'function') {
        scrollStartEventCallbacks.set(this.viewTag, callback);
      } else {
        scrollStartEventCallbacks.delete(this.viewTag);
      }
    }
    onScrollEndEvent(callback) {
      if (typeof callback === 'function') {
        scrollEndEventCallbacks.set(this.viewTag, callback);
      } else {
        scrollEndEventCallbacks.delete(this.viewTag);
      }
    }
    onScrollToLowerEvent() {}
    onScrollToUpperEvent() {}
    onRefresherPullingEvent(callback) {
      if (typeof callback === 'function') {
        refresherPullingCallbacks.set(this.viewTag, callback);
      } else {
        refresherPullingCallbacks.delete(this.viewTag);
      }
    }
    onRefresherWillRefreshEvent(callback) {
      if (typeof callback === 'function') {
        refresherWillRefreshCallbacks.set(this.viewTag, callback);
      } else {
        refresherWillRefreshCallbacks.delete(this.viewTag);
      }
    }
    onRefresherRefreshEvent(callback) {
      if (typeof callback === 'function') {
        refresherRefreshCallbacks.set(this.viewTag, callback);
      } else {
        refresherRefreshCallbacks.delete(this.viewTag);
      }
    }
    onRefresherRestoreEvent(callback) {
      if (typeof callback === 'function') {
        refresherRestoreCallbacks.set(this.viewTag, callback);
      } else {
        refresherRestoreCallbacks.delete(this.viewTag);
      }
    }
    onRefresherAbortEvent(callback) {
      if (typeof callback === 'function') {
        refresherAbortCallbacks.set(this.viewTag, callback);
      } else {
        refresherAbortCallbacks.delete(this.viewTag);
      }
    }
    onRefresherStatusChangeEvent(callback) {
      if (typeof callback === 'function') {
        refresherStatusCallbacks.set(this.viewTag, callback);
      } else {
        refresherStatusCallbacks.delete(this.viewTag);
      }
    }
    setIntersectionListener(options, callback) {
      if (this._intersection && typeof native.removeIntersectionObserver === 'function') {
        native.removeIntersectionObserver(this.windowId, this.viewTag,
                                          this._intersection.uniqueId);
      }
      this._intersection = null;
      if (typeof callback !== 'function') return;
      const uniqueId = ++nextIntersectionId;
      const opts = options || {};
      const rootMargins = opts.rootMargin || [0, 0, 0, 0];
      const thresholds = opts.thresholds || [0];
      let rootId = 0;
      if (opts.root && typeof opts.root === 'object') {
        if (typeof opts.root.viewTag === 'number') {
          rootId = opts.root.viewTag;
        } else if (opts.root.$$ && typeof opts.root.$$.viewTag === 'number') {
          rootId = opts.root.$$.viewTag;
        }
      } else if (typeof opts.root === 'number') {
        rootId = opts.root;
      }
      this._intersection = { uniqueId, callback };
      if (typeof native.addIntersectionObserver === 'function') {
        native.addIntersectionObserver(
          this.windowId, this.viewTag, uniqueId, rootId,
          Number(rootMargins[0]) || 0, Number(rootMargins[1]) || 0,
          Number(rootMargins[2]) || 0, Number(rootMargins[3]) || 0,
          Array.isArray(thresholds) ? thresholds.map(Number) : [0]);
      }
    }
    release() {
      bridgeElements.delete(this.viewTag);
      heroElements.delete(this.viewTag);
      scrollEventCallbacks.delete(this.viewTag);
      swiperScrollEventCallbacks.delete(this.viewTag);
      scrollStartEventCallbacks.delete(this.viewTag);
      scrollEndEventCallbacks.delete(this.viewTag);
      refresherPullingCallbacks.delete(this.viewTag);
      refresherWillRefreshCallbacks.delete(this.viewTag);
      refresherRefreshCallbacks.delete(this.viewTag);
      refresherRestoreCallbacks.delete(this.viewTag);
      refresherAbortCallbacks.delete(this.viewTag);
      refresherStatusCallbacks.delete(this.viewTag);
      if (this._intersection && typeof native.removeIntersectionObserver === 'function') {
        native.removeIntersectionObserver(this.windowId, this.viewTag,
                                          this._intersection.uniqueId);
      }
      this._intersection = null;
      this.ops.push({ op: 'release', id: this.viewTag });
    }
  }


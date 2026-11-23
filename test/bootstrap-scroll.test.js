'use strict';

// D7: scroll events reach the appservice JS bridge.
//
// `native/src/bootstrap.js` is embedded into the addon via `bootstrap_js.h`.
// This test evaluates it in a VM with a minimal `__skylineNative` mock and
// exercises the paths that the DevTools appservice uses:
//
//   * the native scroll sink (set by `native.setScrollSink`) dispatches
//     `onScroll`/`onScrollStart`/`onScrollEnd` to both the plain JS callback
//     registered by the builtin scroll-view (`node.onScrollEvent`) and the
//     worklet registry (`ReanimatedModule.registerEventHandler`);
//   * `PageContext.render(callback)` invokes the setData completion callback,
//     which the address-book example relies on to measure its A-Z offsets.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const repoRoot = path.resolve(__dirname, '..');
const bootstrapSource = fs.readFileSync(
  path.join(repoRoot, 'native', 'src', 'bootstrap.js'), 'utf8');

function loadBootstrap() {
  const sink = { value: null };
  const widgetSink = { value: null };
  const jsonSink = { value: null };
  const httpSink = { value: null };
  const httpCompletions = [];
  const calls = [];
  const appliedOps = [];
  const native = {
    log() {},
    debugInfo() { return {}; },
    setCallback() {},
    applyPageOps(windowId, json) { appliedOps.push({ windowId, json }); },
    getNodeRect() { return { left: 0, top: 0, width: 0, height: 0 }; },
    scrollNodeTo() {},
    dispatchTouch() {},
    dispatchWheel() {},
    onWindowRasterized() {},
    setScrollSink(callback) { sink.value = callback; },
    setWidgetEventSink(callback) { widgetSink.value = callback; },
    setJsonEventSink(callback) { jsonSink.value = callback; },
    setHttpRequestSink(callback) { httpSink.value = callback; },
    httpRequestComplete(requestId, bytes) {
      httpCompletions.push({ requestId, length: bytes ? bytes.length : 0 });
    },
    swiperTo(...args) { calls.push(['swiperTo', ...args]); },
    pickerColumnSetCurrent(...args) {
      calls.push(['pickerColumnSetCurrent', ...args]);
    },
    inputFocus(...args) { calls.push(['inputFocus', ...args]); },
    addIntersectionObserver(...args) {
      calls.push(['addIntersectionObserver', ...args]);
    },
    removeIntersectionObserver(...args) {
      calls.push(['removeIntersectionObserver', ...args]);
    },
    selectionSetRange(...args) { calls.push(['selectionSetRange', ...args]); },
    refresherTriggered(...args) {
      calls.push(['refresherTriggered', ...args]);
    },
    setNavigateBackInterception(...args) {
      calls.push(['setNavigateBackInterception', ...args]);
    },
    setAccessibility(...args) { calls.push(['setAccessibility', ...args]); },
    accessibilityState() { return 3; },
    dumpElementTree(...args) { calls.push(['dumpElementTree', ...args]); },
    dumpRenderTree(...args) { calls.push(['dumpRenderTree', ...args]); },
    collectGarbage(...args) { calls.push(['collectGarbage', ...args]); },
    sendMessageToRunner(...args) {
      calls.push(['sendMessageToRunner', ...args]);
    },
    takeAppSnapshot(...args) { calls.push(['takeAppSnapshot', ...args]); },
    snapshotNode(...args) { calls.push(['snapshotNode', ...args]); },
    setUserGesture(...args) { calls.push(['setUserGesture', ...args]); },
    navigatorPop(...args) { calls.push(['navigatorPop', ...args]); },
    startUserGesture(...args) { calls.push(['startUserGesture', ...args]); },
    stopUserGesture(...args) { calls.push(['stopUserGesture', ...args]); },
    updateAnimationController(...args) {
      calls.push(['updateAnimationController', ...args]);
    },
    relaunchApp(...args) { calls.push(['relaunchApp', ...args]); },
    shutdownApplication(...args) {
      calls.push(['shutdownApplication', ...args]);
    },
  };
  const sandbox = {
    __skylineNative: native,
    console,
    // Run scheduled work immediately: the bootstrap only uses timers for
    // coalescing flushes and the setData render callback.
    setTimeout(callback) { callback(); return 0; },
    clearTimeout() {},
    setInterval() { return { unref() {} }; },
    clearInterval() {},
  };
  vm.createContext(sandbox);
  vm.runInContext(bootstrapSource, sandbox, { filename: 'bootstrap.js' });
  return { sandbox, sink, widgetSink, jsonSink, httpSink, httpCompletions, calls, appliedOps };
}

test('bootstrap: native scroll events dispatch to the window and worklet registrations', () => {
  const { sandbox, sink } = loadBootstrap();
  assert.equal(typeof sink.value, 'function', 'bootstrap installs setScrollSink');

  const SkylineGlobal = sandbox.SkylineGlobal;
  assert.ok(SkylineGlobal, 'SkylineGlobal is published');

  const context = new SkylineGlobal.PageContext(4, 4);
  const element = context.createElement('empty', 'scrollview');
  const calls = [];
  const startCalls = [];
  element.onScrollEvent((...args) => calls.push(args));
  element.onScrollStartEvent(() => startCalls.push('start'));

  const workletEvents = [];
  SkylineGlobal.ReanimatedModule.registerEventHandler(
    `onScroll-${element.viewTag}`, (event) => workletEvents.push(event));

  sink.value({
    nodeId: element.viewTag, phase: 1,
    scrollTop: 42, scrollLeft: 0, scrollWidth: 300, scrollHeight: 900,
    deltaX: 0, deltaY: 12, isDrag: true, boundaryVelocity: 1.5,
  });
  assert.deepEqual(calls, [[42, 0, 300, 900, 0, 12]],
    'bindscroll callback receives the six scroll values');
  assert.equal(workletEvents.length, 1);
  assert.equal(workletEvents[0].type, 'scroll');
  assert.equal(workletEvents[0].detail.scrollTop, 42);
  assert.equal(workletEvents[0].detail.deltaY, 12);
  assert.equal(workletEvents[0].isDrag, true);

  sink.value({
    nodeId: element.viewTag, phase: 0,
    scrollTop: 0, scrollLeft: 0, scrollWidth: 300, scrollHeight: 900,
    deltaX: 0, deltaY: 0, isDrag: true, boundaryVelocity: 0,
  });
  assert.deepEqual(startCalls, ['start'], 'scroll-start phase reaches its callback');

  // The last offset is exposed for selector-query getScrollOffset().
  const offsets = sandbox.__skylineDebug.scrollOffsets.get(element.viewTag);
  assert.equal(offsets.scrollTop, 0);
});

test('bootstrap: PageContext.render invokes the setData completion callback', () => {
  const { sandbox, appliedOps } = loadBootstrap();
  const context = new sandbox.SkylineGlobal.PageContext(4, 4);
  context.createElement('empty', 'view');
  let called = 0;
  context.render(() => { called += 1; });
  assert.equal(called, 1, 'setData callback runs through render()');
  assert.equal(appliedOps.length, 1, 'pending ops are flushed before the callback');
  assert.equal(appliedOps[0].windowId, 4);
  assert.match(appliedOps[0].json, /createContext/);
});

test('bootstrap: intersection/selection/refresher/navigate-back events dispatch', () => {
  const { sandbox, widgetSink, jsonSink, calls } = loadBootstrap();
  assert.equal(typeof jsonSink.value, 'function',
    'bootstrap installs setJsonEventSink');
  const SkylineGlobal = sandbox.SkylineGlobal;
  const context = new SkylineGlobal.PageContext(4, 4);

  // IntersectionObserver: native registration + entry callback.
  const target = context.createElement('empty', 'view');
  const entries = [];
  target.setIntersectionListener(
    { rootMargin: [0, 0, 0, 0], thresholds: [0, 0.5, 1] },
    (entry) => entries.push(entry));
  const add = calls.find((call) => call[0] === 'addIntersectionObserver');
  assert.ok(add, 'native intersection observer registered');
  assert.equal(add[1], 4);
  assert.equal(add[2], target.viewTag);
  assert.equal(add[3], 1);
  assert.equal(add[4], 0);
  assert.equal(add[9].join(','), '0,0.5,1');
  const uniqueId = add[3];
  jsonSink.value(JSON.stringify({
    type: 'intersection',
    nodeId: target.viewTag,
    uniqueId: uniqueId,
    boundingClientRect: { top: 0, right: 100, bottom: 100, left: 0 },
    intersectionRect: { top: 0, right: 100, bottom: 50, left: 0 },
    rootBounds: { top: 0, right: 320, bottom: 240, left: 0 },
    intersectionRatio: 0.5,
    isIntersecting: true,
  }));
  assert.equal(entries.length, 1);
  assert.equal(entries[0].intersectionRatio, 0.5);
  assert.equal(entries[0].isIntersecting, true);
  assert.equal(entries[0].boundingClientRect.width, 100);
  target.setIntersectionListener({}, null);
  assert.ok(calls.some((call) => call[0] === 'removeIntersectionObserver'));

  // Appservice SDK path: node.createIntersectionObserver(...) gets entries as
  // a one-element array.
  const legacy = context.createElement('empty', 'view');
  const legacyEntries = [];
  const handle = legacy.createIntersectionObserver(
    null, [0, 0, 0, 0], [0], (entries) => legacyEntries.push(entries));
  const legacyAdd = calls.filter(
    (call) => call[0] === 'addIntersectionObserver').pop();
  jsonSink.value(JSON.stringify({
    type: 'intersection', nodeId: legacy.viewTag, uniqueId: legacyAdd[3],
    boundingClientRect: { top: 0, right: 10, bottom: 10, left: 0 },
    intersectionRect: { top: 0, right: 10, bottom: 10, left: 0 },
    rootBounds: { top: 0, right: 320, bottom: 240, left: 0 },
    intersectionRatio: 1, isIntersecting: true,
  }));
  assert.equal(legacyEntries.length, 1);
  assert.equal(legacyEntries[0][0].intersectionRatio, 1);
  handle.disconnect();

  // Refresher callbacks registered by wx-scroll-view.
  const scrollView = context.createElement('empty', 'scrollview');
  const refresh = [];
  scrollView.onRefresherRefreshEvent((dy) => refresh.push(dy));
  widgetSink.value({
    nodeId: scrollView.viewTag, kind: 10, value: 55, extra: 0, flag: 0,
  });
  assert.deepEqual([...refresh], [55]);
  assert.equal(sandbox.__skylineDebug.refresherEvents.pop().type, 'refresh');

  // Selection change (DOM dispatch needs a live exparser; the event is
  // recorded regardless).
  const selection = context.createElement('selection', 'selection');
  jsonSink.value(JSON.stringify({
    type: 'selection', nodeId: selection.viewTag,
    left: 1, top: 2, right: 3, bottom: 4, selectedString: 'hello',
  }));
  const selectionEvent = sandbox.__skylineDebug.selectionEvents.pop();
  assert.equal(selectionEvent.selectedString, 'hello');

  // Navigate-back interception is forwarded and recorded.
  sandbox.__skylineDebug.setNavigateBackInterception(4, 1, true);
  jsonSink.value(JSON.stringify({
    type: 'navigateBack', pageId: 1, enabled: true,
  }));
  assert.equal(sandbox.__skylineDebug.navigateBackIntercepts.pop().pageId, 1);

  // Programmatic helpers forward to the addon bridge.
  sandbox.__skylineDebug.selectionSetRange(4, selection.viewTag, 4, 0, 4, 5);
  sandbox.__skylineDebug.refresherTriggered(4, scrollView.viewTag, true, false);
  const forwarded = calls.slice(-3);
  assert.equal(JSON.stringify(forwarded[1]),
    JSON.stringify(['selectionSetRange', 4, selection.viewTag, 4, 0, 4, 5]));
  assert.equal(JSON.stringify(forwarded[2]),
    JSON.stringify(['refresherTriggered', 4, scrollView.viewTag, true, false]));
  assert.equal(JSON.stringify(forwarded[0]),
    JSON.stringify(['setNavigateBackInterception', 4, 1, true]));
});
test('bootstrap: swiper/picker/input widget events reach their registrations', () => {
  const { sandbox, widgetSink, calls } = loadBootstrap();
  assert.equal(typeof widgetSink.value, 'function',
    'bootstrap installs setWidgetEventSink');
  const SkylineGlobal = sandbox.SkylineGlobal;
  const context = new SkylineGlobal.PageContext(4, 4);

  // swiper: change goes to the builtin component callback and worklet.
  const swiper = context.createElement('swiper', 'swiper');
  const changes = [];
  swiper.onChangeEvent((current, reason) => changes.push([current, reason]));
  const changeWorklets = [];
  SkylineGlobal.ReanimatedModule.registerEventHandler(
    `onChange-${swiper.viewTag}`, (event) => changeWorklets.push(event));
  widgetSink.value({
    nodeId: swiper.viewTag, kind: 0, value: 2, extra: 1, flag: 0,
  });
  assert.equal(JSON.stringify(changes), JSON.stringify([[2, 1]]));
  assert.equal(swiper.attrs.current, '2');
  assert.equal(changeWorklets.length, 1);
  assert.equal(changeWorklets[0].detail.current, 2);
  assert.equal(changeWorklets[0].detail.source, 'autoplay');

  // swiper scroll: single translate-pixel argument plus onTransition worklet.
  const scrolls = [];
  swiper.onScrollEvent((delta) => scrolls.push(delta));
  const transitions = [];
  SkylineGlobal.ReanimatedModule.registerEventHandler(
    `onTransition-${swiper.viewTag}`, (event) => transitions.push(event));
  widgetSink.value({
    nodeId: swiper.viewTag, kind: 1, value: 40, extra: 1, flag: 2,
  });
  assert.deepEqual([...scrolls], [40]);
  assert.equal(transitions.length, 1);
  assert.deepEqual([transitions[0].detail.dx, transitions[0].detail.dy], [40, 0]);
  assert.equal(transitions[0].detail.current, 2);

  // picker-view-column: change is recorded and the parent value array built.
  const pickerView = context.createElement('empty', 'picker-view');
  const column = context.createElement('picker-view-column', 'picker-view-column');
  column.attrs.value = '0';
  pickerView.appendChild(column);
  widgetSink.value({
    nodeId: column.viewTag, kind: 2, value: 3, extra: 1, flag: 0,
  });
  const pickerChange = sandbox.__skylineDebug.pickerEvents.pop();
  assert.equal(pickerChange.type, 'change');
  assert.equal(pickerChange.nodeId, column.viewTag);
  assert.equal(pickerChange.detail.value.join(','), '3');
  assert.equal(column.attrs.value, '3');

  // input focus/blur are recorded (DOM dispatch needs a live exparser).
  const input = context.createElement('text', 'input');
  widgetSink.value({ nodeId: input.viewTag, kind: 5, value: 0, extra: 0, flag: 0 });
  widgetSink.value({ nodeId: input.viewTag, kind: 6, value: 0, extra: 0, flag: 0 });
  assert.equal(JSON.stringify(sandbox.__skylineDebug.inputEvents),
    JSON.stringify([
      { type: 'focus', nodeId: input.viewTag },
      { type: 'blur', nodeId: input.viewTag },
    ]));

  // Programmatic helpers forward to the addon bridge.
  sandbox.__skylineDebug.swiperTo(4, swiper.viewTag, 1, false);
  sandbox.__skylineDebug.pickerColumnSetCurrent(4, column.viewTag, 2);
  sandbox.__skylineDebug.inputFocus(4, input.viewTag, true);
  assert.equal(JSON.stringify(calls), JSON.stringify([
    ['swiperTo', 4, swiper.viewTag, 1, false],
    ['pickerColumnSetCurrent', 4, column.viewTag, 2],
    ['inputFocus', 4, input.viewTag, true],
  ]));
});

test('bootstrap: D10 accessibility and automation helpers bridge to native', () => {
  const { sandbox, calls, jsonSink } = loadBootstrap();
  const debug = sandbox.__skylineDebug;
  assert.equal(typeof jsonSink.value, 'function');

  debug.setAccessibility(4, true);
  assert.equal(debug.accessibilityState(), 3);
  debug.dumpElementTree(4);
  debug.dumpRenderTree(4);
  debug.collectGarbage(4);
  debug.sendMessageToRunner(4, 42);
  debug.takeAppSnapshot(4, 7, 2, false);
  debug.snapshotNode(4, 9, 4, 8);
  debug.setUserGesture(4, true);
  debug.relaunchApp(4, 1);
  debug.shutdownApplication(4);

  assert.equal(JSON.stringify(calls), JSON.stringify([
    ['setAccessibility', 4, true],
    ['dumpElementTree', 4],
    ['dumpRenderTree', 4],
    ['collectGarbage', 4],
    ['sendMessageToRunner', 4, '42'],
    ['takeAppSnapshot', 4, 7, 2, false],
    ['snapshotNode', 4, 9, 4, 8],
    ['setUserGesture', 4, true],
    ['relaunchApp', 4, 1],
    ['shutdownApplication', 4],
  ]));

  // Rich events from the native JSON channel are recorded for DevTools.
  jsonSink.value('{"type":"appSnapshot","requestId":7,"byteLength":4,' +
    '"format":"png","width":10,"height":10,"data":"AAAA"}');
  jsonSink.value('{"type":"nodeSnapshot","requestId":8,"byteLength":4,' +
    '"format":"rgba","data":"AAAA"}');
  jsonSink.value('{"type":"accessibilityFeatures","boldText":false}');
  jsonSink.value('{"type":"userGesture","inProgress":true}');
  assert.equal(debug.snapshotEvents.length, 2);
  assert.equal(debug.snapshotEvents[0].requestId, 7);
  assert.equal(debug.accessibilityEvents.length, 1);
  assert.equal(debug.accessibilityEvents[0].boldText, false);
  assert.equal(debug.userGestureEvents.length, 1);
  assert.equal(debug.userGestureEvents[0].inProgress, true);
});

test('bootstrap: HTTP requests proxy through the SkylineShell callback', async () => {
  const { sandbox, httpSink, httpCompletions } = loadBootstrap();
  assert.equal(typeof httpSink.value, 'function',
    'bootstrap installs setHttpRequestSink');

  const shell = new sandbox.SkylineShell();
  const requests = [];
  shell.setHttpRequestCallback((request) => {
    requests.push(request);
    return Promise.resolve(new Uint8Array([1, 2, 3]));
  });

  httpSink.value({ requestId: 42, url: 'https://res.wx.qq.com/a.jpg' });
  await Promise.resolve();
  assert.equal(JSON.stringify(requests),
    JSON.stringify([{ request_id: 42, url: 'https://res.wx.qq.com/a.jpg' }]));

  // The DevTools callback invokes notifyHttpRequestComplete with the bytes.
  shell.notifyHttpRequestComplete(42, 0, 200, {}, new Uint8Array([1, 2, 3]));
  assert.deepEqual(httpCompletions, [{ requestId: 42, length: 3 }]);

  // A missing callback completes with an empty payload so native falls back.
  const second = loadBootstrap();
  second.httpSink.value({ requestId: 43, url: 'https://example.invalid/b.jpg' });
  assert.deepEqual(second.httpCompletions,
    [{ requestId: 43, length: 0 }]);
});

test('bootstrap: custom-route worklets pop and animate the Flutter route', () => {
  const { sandbox, jsonSink, calls } = loadBootstrap();
  const module = sandbox.workletUIModule;
  assert.ok(module, 'workletUIModule is installed');

  // `wx.router.getRouteContext()` worklets call these before/while dragging and
  // when they finish (`didPop`).  They must reach the native addon, otherwise
  // the page cannot be swiped away.
  module.navigatorPop(4, 63);
  module.startUserGesture(4);
  module.stopUserGesture(4);
  module.updateAnimationController(4, 63, 0.5);
  // assets/app.js worklets reference bare `_`-prefixed globals.
  sandbox._navigatorPop(4, 64);
  sandbox._startUserGesture(4);
  sandbox._stopUserGesture(4);
  sandbox._updateAnimationController(4, 64, 0.25);
  assert.deepEqual(calls, [
    ['navigatorPop', 4, 63],
    ['startUserGesture', 4],
    ['stopUserGesture', 4],
    ['updateAnimationController', 4, 63, 0.5],
    ['navigatorPop', 4, 64],
    ['startUserGesture', 4],
    ['stopUserGesture', 4],
    ['updateAnimationController', 4, 64, 0.25],
  ]);

  // Gesture pops are reported to the shell so the DevTools simulator route
  // stack follows the renderer, and the JS mirror pops with it.
  const shell = new sandbox.SkylineShell();
  const backEvents = [];
  const doneEvents = [];
  shell.setNavigateBackCallback((event) => backEvents.push(event));
  shell.setNavigateBackDoneCallback((event) => doneEvents.push(event));
  const context = new sandbox.SkylineGlobal.PageContext(4, 4);
  const first = context.createElement('empty', 'view');
  const second = context.createElement('empty', 'view');
  context.attach(first);
  context.attach(second);
  assert.equal(sandbox.__skylineDebug.attachedRoots.get(4), second);

  jsonSink.value(JSON.stringify(
    { type: 'navigateBackByGesture', windowId: 4, pageId: 63 }));
  assert.equal(JSON.stringify(backEvents), JSON.stringify(
    [{ window_id: 4, page_id: 63 }]));
  assert.equal(sandbox.__skylineDebug.attachedRoots.get(4), first,
    'hit test follows the revealed page');

  jsonSink.value(JSON.stringify(
    { type: 'navigateBackDoneByGesture', windowId: 4, pageId: 63 }));
  assert.equal(JSON.stringify(doneEvents), JSON.stringify(
    [{ window_id: 4, page_id: 63 }]));
});

test('bootstrap: HandlePageRoute drives the custom-route animation worklet', () => {
  const { sandbox, jsonSink } = loadBootstrap();
  const updates = [];
  sandbox.SkylineGlobal.ReanimatedModule.registerEventHandler(
    'onAnimationChange-77', (controller) => updates.push(controller));

  jsonSink.value(JSON.stringify({
    type: 'pageRoute', name: 'onAnimationChange-77', value: 0.4, status: 1,
  }));
  // Status changes arrive with a `status-` prefix on the same handler name.
  jsonSink.value(JSON.stringify({
    type: 'pageRoute', name: 'status-onAnimationChange-77', value: 1, status: 3,
  }));
  assert.equal(JSON.stringify(updates), JSON.stringify([
    { value: 0.4, status: 1 },
    { value: 1, status: 3 },
  ]));
});

test('bootstrap: gesture handlers dispatch drag worklets for custom routes', () => {
  const { sandbox } = loadBootstrap();
  // Every node covers the whole simulator page; the fallback handler lookup
  // uses the published rects.
  sandbox.__skylineNative.getNodeRect = () => ({
    left: 0, top: 0, right: 390, bottom: 844, width: 390, height: 844,
    zIndex: 0, pointerEvents: 0,
  });
  const SkylineGlobal = sandbox.SkylineGlobal;
  assert.ok(SkylineGlobal.GestureHandlerModule,
    'SkylineGlobal exposes GestureHandlerModule');
  const context = new SkylineGlobal.PageContext(4, 4);
  const target = context.createElement('empty', 'view');
  context.attach(target);

  const events = [];
  SkylineGlobal.ReanimatedModule.registerEventHandler(
    'onGestureHandlerEvent-7', (event) => events.push(event));
  SkylineGlobal.GestureHandlerModule.attachGestureHandler(7, 3, {}, target);

  const shell = new sandbox.SkylineShell();
  shell.dispatchTouchStartEvent(4, 10, 400);
  shell.dispatchTouchMoveEvent(4, 40, 400);
  shell.dispatchTouchMoveEvent(4, 200, 404);
  shell.dispatchTouchEndEvent(4, 320, 404);

  // possible -> begin -> active -> end.  The rightward drag is what the
  // card-transition detail page turns into `didPop()`.
  const states = events.map((event) => event.state);
  assert.deepEqual(states, [0, 1, 2, 2, 3]);
  assert.equal(events[1].absoluteX, 40);
  assert.ok(events[2].deltaX > 0, 'active event carries the frame delta');
  assert.ok(events[4].velocityX > 0, 'end event carries the fling velocity');

  SkylineGlobal.GestureHandlerModule.dropGestureHandler(7);
  const dropped = events.length;
  shell.dispatchTouchStartEvent(4, 10, 400);
  shell.dispatchTouchEndEvent(4, 10, 400);
  assert.equal(events.length, dropped, 'dropped handlers stop receiving events');
});

test('bootstrap: swiper drags do not bubble to an outer pan gesture handler', () => {
  const { sandbox } = loadBootstrap();
  const fullPage = {
    left: 0, top: 0, right: 390, bottom: 844, width: 390, height: 844,
    zIndex: 0, pointerEvents: 0,
  };
  const rects = new Map();
  sandbox.__skylineNative.getNodeRect = (windowId, viewTag) =>
    rects.get(viewTag) || fullPage;

  const SkylineGlobal = sandbox.SkylineGlobal;
  const context = new SkylineGlobal.PageContext(4, 4);
  // card_transition detail: <pan-gesture-handler><view>...<swiper><swiper-item>
  // <image/></swiper-item></swiper>...text...</view></pan-gesture-handler>
  const wrapper = context.createElement('empty', 'view');
  const swiper = context.createElement('swiper', 'swiper');
  const item = context.createElement('swiper-item', 'swiper-item');
  const image = context.createElement('image', 'image');
  item.appendChild(image);
  swiper.appendChild(item);
  wrapper.appendChild(swiper);
  context.attach(wrapper);
  const swiperRect = {
    left: 0, top: 0, right: 390, bottom: 520, width: 390, height: 520,
    zIndex: 0, pointerEvents: 0,
  };
  for (const node of [swiper, item, image]) rects.set(node.viewTag, swiperRect);
  rects.set(wrapper.viewTag, fullPage);

  const events = [];
  SkylineGlobal.ReanimatedModule.registerEventHandler(
    'onGestureHandlerEvent-9', (event) => events.push(event));
  SkylineGlobal.GestureHandlerModule.attachGestureHandler(9, 3, {}, wrapper);
  const shell = new sandbox.SkylineShell();
  const states = () => events.map((event) => event.state);
  const rightDrag = (x0, y0) => {
    shell.dispatchTouchStartEvent(4, x0, y0);
    shell.dispatchTouchMoveEvent(4, x0 + 30, y0 + 2);
    shell.dispatchTouchMoveEvent(4, x0 + 190, y0 + 4);
    shell.dispatchTouchEndEvent(4, x0 + 310, y0 + 4);
  };

  // Horizontal swipe on the slideshow: the swiper pages, the outer pan handler
  // (custom-route back) must not start.  Only the POSSIBLE state is emitted.
  rightDrag(10, 300);
  assert.deepEqual(states(), [0],
    'swiper swipe must not reach the route-back worklet');

  // A perpendicular drag over the swiper still bubbles (page scroll / pan).
  events.length = 0;
  shell.dispatchTouchStartEvent(4, 200, 300);
  shell.dispatchTouchMoveEvent(4, 202, 330);
  shell.dispatchTouchMoveEvent(4, 205, 500);
  shell.dispatchTouchEndEvent(4, 205, 640);
  assert.deepEqual(states(), [0, 1, 2, 2, 3],
    'vertical drags over a horizontal swiper still bubble');

  // A horizontal drag outside the swiper keeps driving the route back gesture.
  events.length = 0;
  rightDrag(10, 700);
  assert.deepEqual(states(), [0, 1, 2, 2, 3],
    'drag outside the swiper must still reach the pan worklet');

  // A vertical swiper consumes vertical drags, not horizontal ones.
  swiper.attrs.vertical = '1';
  events.length = 0;
  rightDrag(10, 300);
  assert.deepEqual(states(), [0, 1, 2, 2, 3],
    'horizontal drags over a vertical swiper still bubble');
  events.length = 0;
  shell.dispatchTouchStartEvent(4, 200, 300);
  shell.dispatchTouchMoveEvent(4, 202, 330);
  shell.dispatchTouchMoveEvent(4, 205, 500);
  shell.dispatchTouchEndEvent(4, 205, 640);
  assert.deepEqual(states(), [0],
    'vertical drags on a vertical swiper are consumed by the swiper');
});

test('bootstrap: shared values drive timing animations to completion', () => {
  const { sandbox } = loadBootstrap();
  const mutable = sandbox.SkylineGlobal.ReanimatedModule.makeMutable(1);
  let callbacks = 0;
  const updater = function () {
    return {
      current: 1,
      onStart() {},
      onFrame() {
        this.current -= 0.5;
        return this.current <= 0;
      },
      callback() { callbacks += 1; },
    };
  };
  updater.__worklet = true;
  mutable.value = updater;
  assert.equal(callbacks, 1, 'animation callback runs when onFrame finishes');
  assert.equal(mutable.value, 0, 'final value is the animation current');
});

test('bootstrap: builtin input keyboard registration surface is complete', () => {
  // `<input>`/`<textarea>` register every keyboard channel in `created`; a
  // missing method aborts component creation with
  // `this.$.input.$$.onKeyboardSelectionChange is not a function`.
  const { sandbox, calls } = loadBootstrap();
  const context = new sandbox.SkylineGlobal.PageContext(4, 4);
  const input = context.createElement('wx-input', 'input');
  const channels = [
    'onKeyboardValueChange',
    'onKeyboardShow',
    'onKeyboardHeightChange',
    'onKeyboardConfirm',
    'onKeyboardComplete',
    'onKeyboardSelectionChange',
    'onKeyboardCompositionStart',
    'onKeyboardCompositionEnd',
    'onKeyboardCompositionUpdate',
    'onKeyboardLineChange',
    'onSetValueCompleted',
  ];
  for (const name of channels) {
    assert.equal(typeof input[name], 'function', `${name} is missing`);
    assert.doesNotThrow(() => input[name](() => {}));
  }
  input.focus();
  input.blur();
  assert.deepEqual(
    calls.filter((call) => call[0] === 'inputFocus'),
    [
      ['inputFocus', 4, input.viewTag, true],
      ['inputFocus', 4, input.viewTag, false],
    ],
  );
});

test('bootstrap: worklet mappers drive applyAnimatedStyle updates', () => {
  // `applyAnimatedStyle(selector, worklet)` registers a mapper with
  // SkylineGlobal.workletModule.startMapper.  The native worklet module used to
  // no-op, so a custom-route drag only applied the final release animation and
  // never followed the finger.
  const { sandbox } = loadBootstrap();
  const SkylineGlobal = sandbox.SkylineGlobal;
  const context = new SkylineGlobal.PageContext(4, 4);
  const root = context.getRootNode();
  const view = context.createElement('empty', 'view');
  view.setId('fake-host');
  view.setClass('card detail-content');
  view.setStyle('width: 10px;');
  root.appendChild(view);
  context.attach(root, null);

  const progress = SkylineGlobal.ReanimatedModule.makeMutable(0);
  // Mirrors the generated mapper: it closes over the shared values lexically
  // and applies the returned style to the resolved nodes.
  const worklet = function (_unused, nodes) {
    const style = `opacity: ${progress.value};`;
    for (const node of nodes || []) {
      sandbox.workletUIModule.updateNodeStyle(node, style);
    }
  };
  worklet._closure = { progress };
  const mapperId = SkylineGlobal.workletModule.startMapper(
    '.detail-content', worklet, [progress], [], { immediate: true, flush: 'async' });
  assert.ok(mapperId > 0, 'startMapper returns a mapper id');

  progress.value = 0.25;
  const styles = context.ops
    .filter((op) => op.op === 'setStyle' && op.id === view.viewTag)
    .map((op) => op.value);
  const last = styles[styles.length - 1];
  assert.ok(last, `no animated style was applied:\n${styles.join('\n')}`);
  assert.match(last, /width: 10px;/, 'inline style is preserved');
  assert.match(last, /opacity: 0\.25;/, 'animated style follows the shared value');

  // The mapper only reacts while registered.
  SkylineGlobal.workletModule.stopMapper(4, mapperId);
  progress.value = 0.75;
  const afterStop = context.ops
    .filter((op) => op.op === 'setStyle' && op.id === view.viewTag)
    .map((op) => op.value);
  assert.equal(afterStop[afterStop.length - 1], last, 'stopped mappers stop updating');

  // Component-scoped selectors: a second card registering `.detail-content`
  // must not repaint the first card's element.
  const host = context.createElement('empty', 'view');
  const other = context.createElement('empty', 'view');
  other.setClass('card detail-content');
  host.appendChild(other);
  root.appendChild(host);
  const otherProgress = SkylineGlobal.ReanimatedModule.makeMutable(0);
  const scopedWorklet = function (_unused, nodes) {
    const style = `opacity: ${otherProgress.value};`;
    for (const node of nodes || []) {
      sandbox.workletUIModule.updateNodeStyle(node, style);
    }
  };
  scopedWorklet._closure = { progress: otherProgress };
  const scopedId = SkylineGlobal.workletModule.startMapper(
    '.detail-content', scopedWorklet, [otherProgress], [], { flush: 'sync' });
  host.addAnimatedStyle(scopedId);
  otherProgress.value = 0.5;
  const styleOf = (element) => {
    const values = context.ops
      .filter((op) => op.op === 'setStyle' && op.id === element.viewTag)
      .map((op) => op.value);
    return values[values.length - 1];
  };
  assert.match(styleOf(other), /opacity: 0?\.5;/, 'scoped target updates');
  // The scoped mapper runs while the harness' synchronous setTimeout is still
  // inside startMapper (before addAnimatedStyle assigns the owner), so the
  // sibling may carry the initial opacity; what matters is that later updates
  // no longer reach it.
  assert.doesNotMatch(styleOf(view) || '', /opacity: 0?\.5;/,
                      'sibling component stays untouched');

  // Real SDK path: `applyAnimatedStyle` resolves the selector itself and
  // registers the mapper on that backend element; startMapper only receives
  // the window id.  The mapper must paint the owner element, not look up a
  // selector named "4".
  const owned = context.createElement('empty', 'view');
  owned.setClass('owned-target');
  root.appendChild(owned);
  const ownedProgress = SkylineGlobal.ReanimatedModule.makeMutable(0);
  const ownedWorklet = function (_unused, nodes) {
    const style = `opacity: ${ownedProgress.value};`;
    for (const node of nodes || []) {
      sandbox.workletUIModule.updateNodeStyle(node, style);
    }
  };
  ownedWorklet._closure = { progress: ownedProgress };
  const ownedId = SkylineGlobal.workletModule.startMapper(
    4, ownedWorklet, [ownedProgress], [], { immediate: false });
  owned.addAnimatedStyle(ownedId);
  // `immediate: false` must not paint the updater's current values at
  // registration (list cards start with opacity/scale 0 and rely on this).
  assert.equal(styleOf(owned), undefined,
               'immediate:false mapper does not run at registration');
  ownedProgress.value = 0.75;
  assert.match(styleOf(owned) || '', /opacity: 0?\.75;/,
               'window-id mapper paints its registered owner');

  // `derived()` mappers have no owner/selector and must still run (the worklet
  // assigns its output shared value itself).
  const source = SkylineGlobal.ReanimatedModule.makeMutable(1);
  const derivedOut = SkylineGlobal.ReanimatedModule.makeMutable(0);
  const derivedWorklet = function () { derivedOut.value = source.value * 3; };
  derivedWorklet._closure = { source, derivedOut };
  SkylineGlobal.workletModule.startMapper(
    4, derivedWorklet, [source], [derivedOut], { flush: 'sync' });
  source.value = 5;
  assert.equal(derivedOut.value, 15, 'selector-less mapper still executes');
});

test('bootstrap: derived getter recomputes from its dependencies', () => {
  // `wx.worklet.derived(processor)` uses `mutable.installGetter` on this
  // platform.  The detail page's `.detail-content` opacity comes from a
  // derived value; a missing getter leaves it at 0 and hides the text.
  const { sandbox } = loadBootstrap();
  const M = sandbox.SkylineGlobal.ReanimatedModule;
  const base = M.makeMutable(2);
  const out = M.makeMutable(0);
  const processor = function () { return base.value * 4; };
  processor._closure = { base };
  out.installGetter(processor, Object.values(processor._closure));
  assert.equal(out.value, 8, 'initial derived value');
  base.value = 3;
  assert.equal(out.value, 12, 'derived recomputes when an input changes');
  let notified = 0;
  out.addListener(() => { notified += 1; });
  base.value = 5;
  assert.equal(out.value, 20);
  assert.ok(notified >= 1, 'derived notifies its listeners');
});

test('bootstrap: share-element hero frames reach the onFrame worklet', () => {
  // Native `HandleHeroOnFrame` forwards the begin/end/current rects and the
  // transition progress; the builtin share-element registered its
  // `worklet:onframe` handler as `onFrame-<viewTag>`.
  const { sandbox, jsonSink } = loadBootstrap();
  const events = [];
  sandbox.SkylineGlobal.ReanimatedModule.registerEventHandler(
    'onFrame-39', (event) => events.push(event));
  jsonSink.value(JSON.stringify({
    type: 'heroFrame',
    hash: 'onFrame-39',
    direction: 1,
    progress: 0.25,
    begin: { left: 0, top: 88, right: 390, bottom: 88 },
    end: { left: 4, top: 91, right: 193, bottom: 423 },
    current: { left: 1, top: 88, right: 340, bottom: 170 },
  }));
  assert.equal(events.length, 1, 'frame callback invoked');
  assert.equal(events[0].direction, 1, 'pop direction');
  assert.equal(events[0].progress, 0.25);
  assert.equal(events[0].begin.width, 390);
  assert.equal(events[0].end.width, 189);
  assert.equal(events[0].current.left, 1);
  assert.equal(sandbox.__skylineDebug.heroFrameEvents.length, 1);
});

test('bootstrap: share-element props are backfilled from component data', () => {
  // glass-easel runs the builtin component's property observers before its
  // backend element exists, so `transition-on-gesture`/`rect-tween-type`/
  // `shuttle-on-*` never reach `setAttributes`.  The flush backfill resends
  // the resolved values from the component data so NewHero gets them.
  const { sandbox, appliedOps } = loadBootstrap();
  const SkylineGlobal = sandbox.SkylineGlobal;
  const context = new SkylineGlobal.PageContext(4, 4);
  const element = context.createElement('share-element', 'share-element');
  element.__wxElement = {
    data: {
      key: 'se-key0',
      transitionOnGesture: true,
      rectTweenType: 'cubic-bezier(0.4, 0.0, 0.2, 1.0)',
      shuttleOnPush: 'from',
      shuttleOnPop: 'to',
    },
  };
  context.flush();
  assert.ok(appliedOps.length > 0, 'flush applies the hero attrs');
  const payload = JSON.parse(appliedOps[appliedOps.length - 1].json).ops;
  const attrs = payload.find((op) => op.op === 'setAttributes');
  assert.ok(attrs, 'setAttributes op emitted');
  assert.deepEqual(attrs.attrs, {
    tag: 'se-key0',
    transitionOnGesture: true,
    rectTweenType: 'cubic-bezier(0.4, 0.0, 0.2, 1.0)',
    shuttleOnPush: 'from',
    shuttleOnPop: 'to',
  });
  const before = appliedOps.length;
  context.flush();
  assert.equal(appliedOps.length, before,
    'unchanged component data is not resent every flush');
});

test('bootstrap: native-view gesture handlers attach through the proxy API', () => {
  // The SDK's gesture-handler component calls
  // `gestureHandlerModule.attachNativeViewGestureHandler` for
  // `<*-drag-gesture-handler native-view="swiper">`; the proxy node's
  // recognizer negotiates with the Flutter widget, so the JS module only has
  // to register the handler (the older scroll-view spelling is
  // `proxyScrollViewGestureHandler`).
  const { sandbox } = loadBootstrap();
  const SkylineGlobal = sandbox.SkylineGlobal;
  assert.equal(typeof SkylineGlobal.gestureHandlerModule
    .attachNativeViewGestureHandler, 'function');
  const context = new SkylineGlobal.PageContext(4, 4);
  const swiper = context.createElement('swiper', 'swiper');
  SkylineGlobal.gestureHandlerModule
    .attachNativeViewGestureHandler(11, 7, { tag: 'swiper' }, swiper);
  const entry = sandbox.__skylineDebug.gestureHandlers.get(11);
  assert.ok(entry, 'handler registered');
  assert.equal(entry.gestureType, 7);
  assert.equal(entry.node, swiper);
  SkylineGlobal.gestureHandlerModule.dropGestureHandler(11);
  assert.equal(sandbox.__skylineDebug.gestureHandlers.has(11), false);
});

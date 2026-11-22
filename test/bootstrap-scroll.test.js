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

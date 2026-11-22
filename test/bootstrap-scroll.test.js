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
  return { sandbox, sink, appliedOps };
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

  // The official PageContext applies each mutation immediately; our bridge
  // batches ops until an explicit flush.  The miniprogram SDK does not call
  // render()/flush() for ordinary setData updates, so coalesce them here.
  const liveFlushTimer = setInterval(() => {
    backfillHeroAttributes();
    for (const context of liveContexts) {
      if (context.rendered && context.ops.length > 0) {
        try {
          context.flush();
        } catch (_error) {}
      }
    }
    if (!scrollMonitorInstalled) {
      scrollMonitorInstalled = installRenderScrollMonitor();
    }
  }, 16);
  if (liveFlushTimer && typeof liveFlushTimer.unref === 'function') {
    liveFlushTimer.unref();
  }

  installWorkletUIModules();
  const workletModuleTimer = setInterval(installWorkletUIModules, 100);
  setTimeout(() => {
    clearInterval(workletModuleTimer);
    installWorkletUIModules();
  }, 20000);

  const gateInstalled = installCreateWindowResponseGate();
  if (typeof native.log === 'function') {
    native.log(`createWindow response gate installed=${gateInstalled}`);
  }
  // Electron loads native modules through the preload/native bridge; the
  // DevTools extensions read these globals from the page world, so publish
  // them on window/globalThis as well as the module's own global.
  const targets = [];
  if (typeof globalThis !== 'undefined') targets.push(globalThis);
  if (typeof window !== 'undefined' && window !== globalThis) targets.push(window);
  if (typeof global !== 'undefined' && global !== globalThis && global !== window) {
    targets.push(global);
  }
  for (const target of targets) {
    target.SkylineShell = SkylineShell;
    target.SkylineDebugInfo = debugInfo;
    target.SkylineGlobal = SkylineGlobal;
  }
})();

  // The DevTools simulator does not implement Skyline native views (map,
  // canvas, video, camera).  The mini-program base library reports that
  // expected simulator limitation from the `<map>` / `<canvas>` component
  // `created` hooks with a plain `console.error`.  The node is still laid out
  // and hit-tested as a placeholder, so the notice is not a mini-program
  // error; keep it out of the simulator console while forwarding every other
  // error unchanged.  The render context is an `app_sub_render` iframe and its
  // console object may be replaced when the frame reloads, so re-check the
  // context frames periodically and re-wrap only when the marker is gone.
  const DEVTOOLS_NATIVE_VIEW_NOTICE =
    '开发者工具暂未支持 Skyline 下的 canvas 组件调试';
  let suppressedNativeViewNotice = false;
  let consoleNoticeFilterLogged = false;

  function contextFrameMaps() {
    const globals = [];
    if (typeof window !== 'undefined') {
      globals.push(window.__global__, window.__global);
    } else if (typeof globalThis !== 'undefined') {
      globals.push(globalThis.__global__, globalThis.__global);
    }
    for (const pageGlobal of globals) {
      const maps = pageGlobal && pageGlobal.contextFrameMaps;
      if (maps && typeof maps.forEach === 'function') return maps;
    }
    return null;
  }

  function installConsoleNoticeFilter(target) {
    let cons = null;
    try {
      cons = target && target.console;
    } catch (_error) {
      return false;
    }
    if (!cons || typeof cons.error !== 'function') return false;
    if (cons.error.__skylineNativeViewNoticeFilter) return true;
    const originalError = cons.error;
    function filteredError(...args) {
      if (typeof args[0] === 'string' &&
          args[0].indexOf(DEVTOOLS_NATIVE_VIEW_NOTICE) !== -1) {
        if (!suppressedNativeViewNotice && typeof native.log === 'function') {
          suppressedNativeViewNotice = true;
          native.log(`suppressed devtools native-view notice: ${args[0]}`);
        }
        return undefined;
      }
      return originalError.apply(this, args);
    }
    filteredError.__skylineNativeViewNoticeFilter = true;
    try {
      cons.error = filteredError;
    } catch (_error) {
      // Cross-origin frames (there should be none here) are left untouched.
      return false;
    }
    return cons.error === filteredError;
  }

  function installConsoleNoticeFilters() {
    try {
      const maps = contextFrameMaps();
      if (maps === null) return;
      let installed = false;
      maps.forEach((frame) => {
        if (!frame || !frame.iframe) return;
        let win = null;
        try {
          win = frame.iframe.contentWindow;
        } catch (_error) {
          return;
        }
        if (win && installConsoleNoticeFilter(win)) installed = true;
      });
      if (installed && !consoleNoticeFilterLogged &&
          typeof native.log === 'function') {
        consoleNoticeFilterLogged = true;
        native.log('console notice filter installed');
      }
    } catch (_error) {
      // The context map is populated asynchronously during startup.
    }
  }

  installConsoleNoticeFilters();
  const consoleNoticeFilterTimer = setInterval(installConsoleNoticeFilters, 250);
  if (consoleNoticeFilterTimer &&
      typeof consoleNoticeFilterTimer.unref === 'function') {
    consoleNoticeFilterTimer.unref();
  }

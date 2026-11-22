'use strict';

const { capture } = require('../lib/util');

function methodNames(shell) {
  return Object.getOwnPropertyNames(Object.getPrototypeOf(shell));
}

function globalDescriptor(name) {
  const descriptor = Object.getOwnPropertyDescriptor(global, name);
  if (!descriptor) return null;
  return {
    enumerable: descriptor.enumerable,
    writable: descriptor.writable,
    configurable: descriptor.configurable,
    valueType: typeof descriptor.value,
  };
}

function setterNames() {
  return [
    'setNotifyBootstrapDoneCallback',
    'setNotifyWindowReadyCallback',
    'setNotifyRouteDoneCallback',
    'setNavigateBackCallback',
    'setNavigateBackDoneCallback',
    'setLoadResourceCallback',
    'setLoadResourceAsyncCallback',
    'setHttpRequestCallback',
    'setSendLogCallback',
  ];
}

function makeShell(ctx) {
  if (!ctx.addon) throw new Error('skyline addon module is not available');
  // The Windows oracle may create an engine window before the case runs; reuse
  // that shell so the observation targets the same runtime instance.
  return ctx.shell || new global.SkylineShell();
}


function createVariants() {
  const handle = () => new Uint8Array(16);
  const table = {
    validTypes: [1, 'bundle', 100, 100, 2, true, null, 'addon', handle()],
    dprString: [1, 'bundle', 100, 100, '2', true, null, 'addon', handle()],
    dprZero: [1, 'bundle', 100, 100, 0, true, null, 'addon', handle()],
    dprOne: [1, 'bundle', 100, 100, 1, true, null, 'addon', handle()],
    dprThree: [1, 'bundle', 100, 100, 3, true, null, 'addon', handle()],
    dprNaN: [1, 'bundle', 100, 100, NaN, true, null, 'addon', handle()],
    widthString: [1, 'bundle', '100', 100, 2, true, null, 'addon', handle()],
    windowIdString: ['1', 'bundle', 100, 100, 2, true, null, 'addon', handle()],
    showWindowString: [1, 'bundle', 100, 100, 2, 'yes', null, 'addon', handle()],
    bundleNumber: [1, 42, 100, 100, 2, true, null, 'addon', handle()],
    addonMissing: [1, 'bundle', 100, 100, 2, true, null],
    descriptorString: [1, 'bundle', 100, 100, 2, true, null, 'addon', 'nope'],
    descriptorShort: [1, 'bundle', 100, 100, 2, true, null, 'addon', new Uint8Array(8)],
    descriptorArray: [1, 'bundle', 100, 100, 2, true, null, 'addon', Array.from(handle())],
    noDescriptor: [1, 'bundle', 100, 100, 2, true, null, 'addon'],
    bufferAndDescriptor: [1, 'bundle', 100, 100, 2, true, new ArrayBuffer(16), 'addon', handle()],
  };
  return Object.entries(table).map(([name, rawArgs]) => {
    const args = rawArgs.slice();
    // The official implementation resolves the bundle and addon relative to
    // the Electron executable unless absolute paths are supplied.
    if (args.length >= 2 && args[1] === 'bundle') args[1] = '__BUNDLE__';
    if (args.length >= 8 && args[7] === 'addon') args[7] = '__ADDON__';
    return ({

    id: `create.${name}`,
    group: 'create',
    isolate: true,
    engine: name === 'validTypes' || name === 'bufferAndDescriptor',
    engineContext: !['addonMissing'].includes(name),
    run(ctx) {
      const shell = makeShell(ctx);
      return capture(() => shell.createWindow(...args.map(arg => { if (arg === '__BUNDLE__') return ctx.bundle || 'bundle'; if (arg === '__ADDON__') return ctx.addonPath || 'addon'; return arg; })));
    },
  });
  });
}

const cases = [
  {
    id: 'module.exports',
    group: 'module',
    run() {
      return {
        exports: ctxExports(),
        debugType: typeof global.SkylineDebugInfo,
        shellType: typeof global.SkylineShell,
        windowType: typeof global.SkylineWindow,
      };
      function ctxExports() {
        // The addon module object is on the context only when the driver can
        // require it; expose it via a global marker set by the driver.
        return Array.isArray(globalThis.__skyline_addon_keys__)
          ? globalThis.__skyline_addon_keys__
          : null;
      }
    },
  },
  {
    id: 'globals.surface',
    group: 'module',
    run() {
      return {
        shellName: global.SkylineShell && global.SkylineShell.name,
        shellLength: global.SkylineShell && global.SkylineShell.length,
        shellOwnKeys: Object.getOwnPropertyNames(global.SkylineShell).sort(),
        shellKeys: Object.keys(global.SkylineShell),
        debugKeys: global.SkylineDebugInfo && Object.keys(global.SkylineDebugInfo).sort(),
        debug: global.SkylineDebugInfo,
        shellDescriptor: globalDescriptor('SkylineShell'),
        debugDescriptor: globalDescriptor('SkylineDebugInfo'),
      };
    },
  },
  {
    id: 'class.prototype',
    group: 'module',
    run(ctx) {
      const shell = makeShell(ctx);
      return {
        instanceOwnKeys: Object.getOwnPropertyNames(shell).sort(),
        prototypeKeys: methodNames(shell),
        prototypeStaticKeys: Object.getOwnPropertyNames(Object.getPrototypeOf(shell)).sort(),
      };
    },
  },
  {
    id: 'class.methods',
    group: 'module',
    run(ctx) {
      const shell = makeShell(ctx);
      const proto = Object.getPrototypeOf(shell);
      const methods = {};
      for (const name of methodNames(shell)) {
        const descriptor = Object.getOwnPropertyDescriptor(proto, name);
        methods[name] = {
          type: typeof shell[name],
          length: typeof descriptor.value === 'function' ? descriptor.value.length : null,
          enumerable: descriptor.enumerable,
          writable: descriptor.writable,
          configurable: descriptor.configurable,
        };
      }
      return methods;
    },
  },
  {
    id: 'construct.rejectsSecondInstanceShape',
    group: 'module',
    run() {
      return {
        withoutNew: capture(() => global.SkylineShell()),
        withArguments: capture(() => new global.SkylineShell(1, 2, 3)),
        withThis: capture(() => global.SkylineShell.call({})),
      };
    },
  },
  {
    id: 'callbacks.nonFunctionArguments',
    group: 'validation',
    run(ctx) {
      const shell = makeShell(ctx);
      const result = {};
      for (const name of setterNames()) {
        result[name] = {
          noArgs: capture(() => shell[name]()),
          number: capture(() => shell[name](42)),
          nul: capture(() => shell[name](null)),
          object: capture(() => shell[name]({})),
          fn: capture(() => shell[name](() => {})),
          extra: capture(() => shell[name](() => {}, 'extra', 1)),
        };
      }
      return result;
    },
  },
  {
    id: 'safeArea.validation',
    group: 'validation',
    run(ctx) {
      const shell = makeShell(ctx);
      return {
        noArgs: capture(() => shell.setSafeAreaEdgeInsets()),
        oneArg: capture(() => shell.setSafeAreaEdgeInsets(1)),
        twoArgs: capture(() => shell.setSafeAreaEdgeInsets(1, 2)),
        fourArgs: capture(() => shell.setSafeAreaEdgeInsets(1, 2, 3, 4)),
        strings: capture(() => shell.setSafeAreaEdgeInsets('1', '2', '3', '4')),
        floats: capture(() => shell.setSafeAreaEdgeInsets(1.5, -2.5, 0.1, 999999)),
        nan: capture(() => shell.setSafeAreaEdgeInsets(NaN, 0, 0, 0)),
        extra: capture(() => shell.setSafeAreaEdgeInsets(1, 2, 3, 4, 5)),
      };
    },
  },
  {
    id: 'brightness.validation',
    engineContext: true,
    group: 'validation',
    run(ctx) {
      const shell = makeShell(ctx);
      return {
        noArgs: capture(() => shell.onPlatformBrightnessChanged()),
        light: capture(() => shell.onPlatformBrightnessChanged(1)),
        dark: capture(() => shell.onPlatformBrightnessChanged(2)),
        three: capture(() => shell.onPlatformBrightnessChanged(3)),
        zero: capture(() => shell.onPlatformBrightnessChanged(0)),
        negative: capture(() => shell.onPlatformBrightnessChanged(-1)),
        string: capture(() => shell.onPlatformBrightnessChanged('dark')),
        nul: capture(() => shell.onPlatformBrightnessChanged(null)),
        float: capture(() => shell.onPlatformBrightnessChanged(1.5)),
      };
    },
  },
  {
    id: 'destroy.validation',
    engineContext: true,
    group: 'validation',
    run(ctx) {
      const shell = makeShell(ctx);
      return {
        noArgs: capture(() => shell.destroyWindow()),
        zero: capture(() => shell.destroyWindow(0)),
        unknown: capture(() => shell.destroyWindow(99)),
        string: capture(() => shell.destroyWindow('x')),
        negative: capture(() => shell.destroyWindow(-1)),
        extra: capture(() => shell.destroyWindow(99, 'extra')),
      };
    },
  },
  {
    id: 'raster.validation',
    group: 'validation',
    run(ctx) {
      const shell = makeShell(ctx);
      return {
        noArgs: capture(() => shell.onWindowRasterized()),
        oneArg: capture(() => shell.onWindowRasterized(1)),
        twoNumbers: capture(() => shell.onWindowRasterized(1, 2)),
        unknownWindow: capture(() => shell.onWindowRasterized(99, () => {})),
        withExtra: capture(() => shell.onWindowRasterized(99, () => {}, 'x')),
      };
    },
  },
  {
    id: 'dispatch.unknownWindow.touchStart',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchTouchStartEvent(917, 1, 2)); },
  },
  {
    id: 'dispatch.unknownWindow.touchMove',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchTouchMoveEvent(917, 1, 2)); },
  },
  {
    id: 'dispatch.unknownWindow.touchEnd',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchTouchEndEvent(917, 1, 2)); },
  },
  {
    id: 'dispatch.unknownWindow.touchOver',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchTouchOverEvent(917, 1, 2)); },
  },
  {
    id: 'dispatch.unknownWindow.touchCancel',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchTouchCancelEvent(917)); },
  },
  {
    id: 'dispatch.unknownWindow.wheel',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchWheelEvent(917, 1, 2, 3, 4)); },
  },
  {
    id: 'dispatch.unknownWindow.char',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchCharEvent(917, 65)); },
  },
  {
    id: 'dispatch.unknownWindow.keyboard',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchKeyboardEvent(917, 65, 30, 1, 0)); },
  },
  {
    id: 'dispatch.unknownWindow.extraArgument',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).dispatchTouchStartEvent(917, 1, 2, 'extra')); },
  },
  {
    id: 'notify.navigateTo',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyNavigateTo(917, 'page', 'url', {})); },
  },
  {
    id: 'notify.redirectTo',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyRedirectTo(917, 'page', 'url', {})); },
  },
  {
    id: 'notify.navigateBack',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyNavigateBack(917, 'page', 1, 'url', {})); },
  },
  {
    id: 'notify.appLaunch',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyAppLaunch(917, 'path', {})); },
  },
  {
    id: 'notify.reLaunch',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyReLaunch(917, 'path', {})); },
  },
  {
    id: 'notify.autoReLaunch',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyAutoReLaunch(917, 'path', {})); },
  },
  {
    id: 'notify.switchTab',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifySwitchTab(917, 'path', {})); },
  },
  {
    id: 'notify.resourceLoad',
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyResourceLoad(917, Buffer.alloc(0))); },
  },
  {
    id: 'notify.httpRequestComplete',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) { return capture(() => makeShell(ctx).notifyHttpRequestComplete(917, 0, 200, {}, Buffer.alloc(0))); },
  },
  {
    id: 'create.argumentCount',
    group: 'validation',
    isolate: true,
    run(ctx) {
      const shell = makeShell(ctx);
      const base = [1, 'bundle', 100, 100, 2, true, null];
      const result = {};
      for (let count = 0; count <= 7; count++) {
        result[`args${count}`] = capture(() => shell.createWindow(...base.slice(0, count)));
      }
      return result;
    },
  },
  ...createVariants(),
  {
    id: 'create.destroyUnknownAfterAttempt',
    engineContext: true,
    group: 'validation',
    isolate: true,
    run(ctx) {
      const shell = makeShell(ctx);
      return {
        before: capture(() => shell.destroyWindow(1)),
        attempt: capture(() => shell.createWindow(1, 'bundle', 100, 100, 2, true, null, 'addon', new Uint8Array(16))),
        after: capture(() => shell.destroyWindow(1)),
      };
    },
  },
];

for (const item of cases) {
  if (!item.group) item.group = 'module';
}

module.exports = cases;

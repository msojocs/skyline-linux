'use strict';

// Records the node-operation stream of the real Skyline renderer bundle.
//
// The DevTools ship the renderer half of the appservice/V8 flow as a JS bundle
// (`skyline-addon/bundle/flutter_assets/assets/app.js`).  Inside the official
// `skyline.node` that bundle runs in an embedded V8 and talks to native code
// through the `SkylineGlobal` bindings (exparser's `documentBackend: 'custom'`
// contract): `PageContext.createElement`, element `setText`/`setAttribute`/
// `setStyle`/`addClass`/`appendChild`/`splice`, `appendStyleSheet`, `attach`,
// `startRender`.
//
// This tool executes the same bundle in Node's V8 with a capture shim and
// writes the resulting operation stream as JSON.  `native/runtime` replays that
// stream through the real `package:skyline` callbacks, which replaces the
// hand-written JSON widget descriptors with the appservice operation flow.
//
// The bundle is proprietary and is not copied into this repository; only the
// captured operation log (structure, not source) is written.
//
// Usage:
//   node tools/record-skyline-ops.js [--bundle <app.js>] [--out <file|->]
//                                    [--timeout-ms 1000] [--quiet]

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const repoRoot = path.resolve(__dirname, '..');

function parseArgs(argv) {
  const options = {
    bundle: process.env.SKYLINE_SKYLINE_BUNDLE || null,
    out: null,
    timeoutMs: 1000,
    quiet: false,
    tapOption: null,
    tapText: null,
    tapDelayMs: 300,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--bundle') options.bundle = argv[++i];
    else if (arg === '--out') options.out = argv[++i];
    else if (arg === '--timeout-ms') options.timeoutMs = Number(argv[++i]) || 1000;
    else if (arg === '--tap-option') options.tapOption = String(argv[++i]);
    else if (arg === '--tap-text') options.tapText = String(argv[++i]);
    else if (arg === '--tap-delay-ms') options.tapDelayMs = Number(argv[++i]) || 300;
    else if (arg === '--quiet') options.quiet = true;
    else if (arg === '-h' || arg === '--help') {
      console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(22, 32).join('\n'));
      process.exit(0);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  return options;
}

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) return candidate;
  }
  return null;
}

function defaultBundle() {
  const resources = [
    process.env.SKYLINE_WIN_RESOURCES,
    path.resolve(repoRoot, '..', 'wechat-web-devtools-linux', 'cache', 'devtools', 'resources', 'app.asar.unpacked'),
    path.resolve(repoRoot, '..', 'wechat-web-devtools-linux', 'tmp', 'official-win-resources', 'resources', 'app.asar.unpacked'),
  ].filter(Boolean);
  const candidates = [];
  for (const resource of resources) {
    const assets = path.join(resource, 'node_modules', 'skyline-addon', 'bundle', 'flutter_assets', 'assets');
    candidates.push(path.join(assets, 'app.js'), path.join(assets, 'app_0.js'));
  }
  return firstExisting(candidates);
}

// Capture shim implementing exparser's custom-backend contract.  It keeps just
// enough state for insertChildInCustom/splice/findChildPosition to work while
// logging every operation the renderer emits.
class CaptureElement {
  constructor(tag, name, ops) {
    this.ops = ops || [];
    this.tag = tag;
    this.name = name;
    this.attrs = {};
    this.children = [];
    this.parent = null;
    this.text = '';
    this._id = '';
    this._style = '';
    this.viewTag = ++CaptureElement.nextTag;
    CaptureElement.all.push(this);
    this.ops.push({ op: 'createElement', id: this.viewTag, tag, name });
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
    this.ops.push({ op: 'setAttribute', id: this.viewTag, name, value: scalar(value) });
  }
  removeAttribute(name) {
    delete this.attrs[name];
    this.ops.push({ op: 'removeAttribute', id: this.viewTag, name });
  }
  setStyle(value) {
    this._style = String(value);
    this.ops.push({ op: 'setStyle', id: this.viewTag, value: this._style });
  }
  addClass(prefix, scope) {
    this.ops.push({ op: 'addClass', id: this.viewTag, prefix, name: scope });
  }
  removeClass(prefix, scope) {
    this.ops.push({ op: 'removeClass', id: this.viewTag, prefix, name: scope });
  }
  setClass(names) {
    this.ops.push({ op: 'setClass', id: this.viewTag, names: String(names) });
  }
  setStyleScope(scope) {
    this.ops.push({ op: 'setStyleScope', id: this.viewTag, scope: String(scope) });
  }
  associateComponent() {
    this.ops.push({ op: 'associateComponent', id: this.viewTag });
  }
  setListenerOption(name, option) {
    this.ops.push({ op: 'setListenerOption', id: this.viewTag, name, option: scalar(option) });
  }
  setAttributes(values) {
    this.ops.push({ op: 'setAttributes', id: this.viewTag, attrs: plain(values) });
  }
  setAttributeValue(name, value) {
    this.ops.push({ op: 'setAttributeValue', id: this.viewTag, name, value: scalar(value) });
  }
  setHitTestBehavior(value) {
    this.ops.push({ op: 'setHitTestBehavior', id: this.viewTag, value });
  }
  addAnimatedStyle() {
    this.ops.push({ op: 'addAnimatedStyle', id: this.viewTag });
  }
  asShadowRoot() {
    this.ops.push({ op: 'asShadowRoot', id: this.viewTag });
    return this;
  }
  forceDetached() {}
  getBoundingClientRect() {
    return { left: 0, top: 0, width: 0, height: 0, right: 0, bottom: 0 };
  }
  getAttribute(name) { return this.attrs[name]; }
  cloneNode() {
    const clone = new CaptureElement(this.tag, this.name, this.ops);
    clone.attrs = { ...this.attrs };
    clone.text = this.text;
    clone._style = this._style;
    this.ops.push({ op: 'cloneNode', id: this.viewTag, clone: clone.viewTag });
    return clone;
  }

  appendChild(child) {
    if (!child) return;
    this.ops.push({ op: 'appendChild', id: this.viewTag, child: idOf(child) });
    this.children.push(child);
    child.parent = this;
  }
  insertBefore(child, before) {
    if (!child) return;
    this.ops.push({
      op: 'insertBefore', id: this.viewTag, child: idOf(child), before: idOf(before),
    });
    const index = this.children.indexOf(before);
    this.children.splice(index < 0 ? this.children.length : index, 0, child);
    child.parent = this;
  }
  removeChild(child) {
    if (!child) return;
    this.ops.push({ op: 'removeChild', id: this.viewTag, child: idOf(child) });
    const index = this.children.indexOf(child);
    if (index >= 0) this.children.splice(index, 1);
    child.parent = null;
  }
  replaceChild(child, index) {
    this.ops.push({ op: 'replaceChild', id: this.viewTag, child: idOf(child), index });
    this.children[index] = child;
    child.parent = this;
  }
  findChildPosition(child) {
    return this.children.indexOf(child);
  }
  splice(position, count, ...items) {
    this.ops.push({
      op: 'splice', id: this.viewTag, pos: position, count,
      items: items.map(idOf),
    });
    if (position < 0 || position > this.children.length) position = this.children.length;
    const flat = [];
    for (const item of items) {
      if (item && item.tag === 'fragment') flat.push(...item.children);
      else if (item !== undefined && item !== null) flat.push(item);
    }
    this.children.splice(position, count, ...flat);
    for (const child of flat) child.parent = this;
  }
  release() {
    this.ops.push({ op: 'release', id: this.viewTag });
  }
}
CaptureElement.nextTag = 0;
CaptureElement.all = [];

class CapturePageContext {
  constructor(id, windowId, ops) {
    this.id = id;
    this.windowId = windowId;
    this.ops = ops;
    ops.push({ op: 'createContext', id, windowId });
  }
  createElement(tag, name) {
    return new CaptureElement(tag, name, this.ops);
  }
  appendStyleSheet(css, index) {
    this.ops.push({ op: 'appendStyleSheet', index, css: String(css) });
  }
  attach(root, host) {
    this.ops.push({ op: 'attach', root: idOf(root), host: idOf(host) });
  }
  attachCustomRoute(...args) {
    this.ops.push({
      op: 'attachCustomRoute',
      args: args.map((arg) => (typeof arg === 'object' ? '[object]' : arg)),
    });
  }
  startRender(callback) {
    this.ops.push({ op: 'startRender' });
    if (typeof callback === 'function') setImmediate(callback);
  }
  setAsTab() {
    this.ops.push({ op: 'setAsTab' });
  }
}

function idOf(value) {
  if (value === undefined || value === null) return 0;
  return typeof value === 'object' ? value.viewTag : value;
}

function scalar(value) {
  if (typeof value === 'string') return value;
  if (value === undefined) return undefined;
  try {
    return JSON.parse(JSON.stringify(value));
  } catch {
    return String(value);
  }
}

function plain(value) {
  if (value === undefined || value === null) return null;
  try {
    return JSON.parse(JSON.stringify(value));
  } catch {
    return null;
  }
}

async function run(bundlePath, options) {
  const ops = [];
  const updates = [];
  CaptureElement.nextTag = 0;
  CaptureElement.all = [];
  const makeContext = (id, windowId) => new CapturePageContext(id, windowId, ops);
  let touchCallback = null;
  let eventCallback = null;
  const Runtime = {
    registerEventCallback(fn) {
      eventCallback = typeof fn === 'function' ? fn : null;
      ops.push({ op: 'registerEventCallback' });
    },
    registerAnimationCallback() { ops.push({ op: 'registerAnimationCallback' }); },
    registerTouchCallback(fn) {
      touchCallback = typeof fn === 'function' ? fn : null;
      ops.push({ op: 'registerTouchCallback' });
    },
    registerKeyCallback() { ops.push({ op: 'registerKeyCallback' }); },
    registerMouseCallback() { ops.push({ op: 'registerMouseCallback' }); },
    registerTransitionCallback() { ops.push({ op: 'registerTransitionCallback' }); },
  };
  const Navigator = {
    notifyNavigateTo(...args) { ops.push({ op: 'notifyNavigateTo', args }); },
    notifySwitchTab(...args) { ops.push({ op: 'notifySwitchTab', args }); },
    notifyNavigateBack(...args) { ops.push({ op: 'notifyNavigateBack', args }); },
  };
  const ReanimatedModule = {
    makeMutable: (value) => ({ value }),
    makeRemote: (value) => value,
    makeShareable: (value) => value,
    registerMutableValueSetter() {},
    startMapper(...args) {
      ops.push({ op: 'startMapper', args: args.map(scalar) });
      return ++CaptureElement.nextTag;
    },
    stopMapper() { ops.push({ op: 'stopMapper' }); },
    registerEventHandler() { return ++CaptureElement.nextTag; },
    unregisterEventHandler() {},
  };

  // PageContext is constructed with `new`, so expose a constructible class whose
  // instances share the capture log.
  class PageContext extends CapturePageContext {
    constructor(id, windowId) { super(id, windowId, ops); }
  }
  // Unknown native binding methods are stubbed and logged instead of aborting
  // the capture; the bundle keeps evolving between DevTools releases.
  function autoStub(name, target) {
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        return (...args) => {
          ops.push({ op: `native:${name}.${property}`, args: args.map(scalar) });
          return ++CaptureElement.nextTag;
        };
      },
    });
  }

  const SkylineGlobal = {
    Runtime: autoStub('Runtime', Runtime),
    Navigator: autoStub('Navigator', Navigator),
    ReanimatedModule: autoStub('ReanimatedModule', ReanimatedModule),
    PageContext,
    TabBarContext: PageContext,
  };

  const quiet = options.quiet;
  const forward = (level) => (...args) => {
    if (!quiet) console.error(`[js ${level}]`, ...args);
  };
  const sandbox = {
    _setGlobalConsole() {},
    _WORKLET: false,
    SkylineGlobal,
    console: {
      log: forward('log'), warn: forward('warn'),
      error: forward('error'), info: forward('info'), debug: forward('debug'),
    },
  };
  sandbox.globalThis = sandbox;
  sandbox.window = sandbox;
  sandbox.self = sandbox;

  const source = fs.readFileSync(bundlePath, 'utf8');
  const context = vm.createContext(sandbox, { name: 'skyline-appservice' });
  vm.runInContext(source, context, { filename: path.basename(bundlePath), timeout: 15000 });

  await new Promise((resolve) => setTimeout(resolve, options.tapDelayMs));

  // Native code normally hit-tests and emits a `tap` event with the exparser
  // target.  In the capture shim `data-option` or the element text identifies
  // the target; using `__wxElement` avoids reimplementing hit testing.
  const hasTap = options.tapOption !== null || options.tapText !== null;
  const dispatchTap = touchCallback || eventCallback;
  if (hasTap && dispatchTap !== null) {
    // `data-*` values live on the exparser element's `dataset`, not on the
    // custom-backend DOM element, so walk the exparser tree exposed by the
    // runtime core rather than the captured operation ids.
    const runtimeCore = sandbox.__runtimeCore;
    const root = runtimeCore && runtimeCore.getRootElement
      ? runtimeCore.getRootElement(0)
      : null;
    const seen = new Set();
    const findTarget = (element) => {
      if (!element || typeof element !== 'object' || seen.has(element)) return null;
      seen.add(element);
      if (element.dataset &&
          options.tapOption !== null &&
          String(element.dataset.option) === String(options.tapOption)) {
        return element;
      }
      if (options.tapText !== null &&
          typeof element.textContent === 'string' &&
          element.textContent.includes(options.tapText)) {
        return element;
      }
      const children = [];
      if (Array.isArray(element.childNodes)) children.push(...element.childNodes);
      if (element.shadowRoot) children.push(element.shadowRoot);
      for (const child of children) {
        const found = findTarget(child);
        if (found) return found;
      }
      return null;
    };
    const wxElement = findTarget(root) ||
      (CaptureElement.all.find(
        (element) => element.attrs && String(element.attrs['data-option'] || '') ===
          String(options.tapOption),
      ) || {}).__wxElement;
    if (wxElement) {
      const x = 50;
      const y = 80;
      const touch = { identifier: 0, pageX: x, pageY: y, clientX: x, clientY: y };
      const before = ops.length;
      dispatchTap({
        type: 'tap',
        target: wxElement,
        touches: [touch],
        changedTouches: [touch],
        timeStamp: Date.now(),
        detail: { x, y },
      });
      // Route handlers are async (`pageReady` resolves in a microtask); let
      // them finish before slicing the update batch.
      await new Promise((resolve) => setImmediate(resolve));
      const updateOps = ops.splice(before);
      if (updateOps.length > 0) {
        updates.push({ afterMs: options.tapDelayMs, ops: updateOps });
      }
    } else if (!options.quiet) {
      console.error('tap target not found (data-option=' + options.tapOption +
        ', text=' + options.tapText + ')');
    }
  }

  await new Promise((resolve) => setTimeout(resolve, 50));
  return { ops, updates };
}

async function main() {
  let options;
  try {
    options = parseArgs(process.argv.slice(2));
  } catch (error) {
    console.error(String(error.message || error));
    process.exit(2);
  }
  const bundle = options.bundle || defaultBundle();
  if (!bundle) {
    console.error(
      'skyline renderer bundle not found; set SKYLINE_SKYLINE_BUNDLE or pass --bundle.\n' +
      'Expected at ../wechat-web-devtools-linux/cache/devtools/resources/app.asar.unpacked/' +
      'node_modules/skyline-addon/bundle/flutter_assets/assets/app.js',
    );
    process.exit(1);
  }
  const result = await run(bundle, options);
  const payload = {
    source: path.basename(bundle),
    page: options.tapOption !== null
      ? `spa-tap-${options.tapOption}`
      : (options.tapText !== null ? `spa-tap-text` : 'spa-page-a'),
    ops: result.ops,
  };
  if (result.updates.length > 0) payload.updates = result.updates;
  const ops = result.ops;
  const text = JSON.stringify(payload, null, 1);
  if (options.out && options.out !== '-') {
    fs.writeFileSync(options.out, text + '\n');
    if (!options.quiet) console.error(`recorded ${ops.length} ops -> ${options.out}`);
  } else if (!options.out) {
    process.stdout.write(text + '\n');
  } else {
    process.stdout.write(text + '\n');
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});

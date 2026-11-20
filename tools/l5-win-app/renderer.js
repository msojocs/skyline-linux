'use strict';
// Captures one L5 fixture frame with the official Windows Skyline engine.
// The engine runs in this Electron renderer; PageContext is the native V8
// binding used by the appservice renderer bundle.
const { ipcRenderer } = require('electron');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const RES = process.env.SKYLINE_WIN_RESOURCES;
const fixturePath = process.env.L5_FIXTURE;
const framePath = process.env.L5_FRAME;
const W = Number(process.env.L5_WIDTH || 320);
const H = Number(process.env.L5_HEIGHT || 240);
const DPR = Number(process.env.L5_DPR || 2);
const WINDOW_ID = Number(process.env.L5_WINDOW_ID || 1);
const KEY = process.env.L5_KEY || `l5win_${process.pid}_${Date.now().toString(36)}`;
const frameSize = W * DPR * H * DPR * 4;
const report = { ok: false, key: KEY, width: W, height: H, dpr: DPR, events: [] };

function log(...args) {
  const line = args.map((a) => (typeof a === 'string' ? a : JSON.stringify(a))).join(' ');
  report.events.push(line);
  ipcRenderer.send('l5-log', { kind: 'renderer', line });
  console.log(line);
}
function finish(ok) {
  report.ok = !!ok;
  ipcRenderer.send('l5-finish', report);
}

function countNonWhite(pixels) {
  let n = 0;
  for (let i = 0; i + 3 < pixels.length; i += 4 * 97) {
    if (pixels[i] !== 255 || pixels[i + 1] !== 255 || pixels[i + 2] !== 255) n++;
  }
  return n;
}

let fixture;
try {
  fixture = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));
} catch (error) {
  log('fixture read error', String(error));
  finish(false);
}

try {
  const shared = require(path.join(RES, 'node_modules', 'sharedMemory', 'sharedMemory.node'));
  require(path.join(RES, 'node_modules', 'skyline-addon', 'build', 'skyline.node'));
  report.skyline = global.SkylineDebugInfo;
  const shell = new global.SkylineShell();
  let started = false;
  let runStarted = false;

  for (const name of [
    'setNotifyBootstrapDoneCallback', 'setNotifyWindowReadyCallback',
    'setNotifyRouteDoneCallback', 'setNavigateBackCallback',
    'setNavigateBackDoneCallback', 'setSendLogCallback',
  ]) {
    try {
      shell[name]((...args) => {
        if (name === 'setNotifyWindowReadyCallback') {
          log('windowReady', ...args);
          start();
        }
      });
    } catch (error) {
      log(name, 'register error', String(error && error.message || error));
    }
  }
  shell.setLoadResourceCallback((request) => {
    const p = typeof request === 'string' ? request : (request && (request.path || request.name));
    log('loadResource', String(p));
    try { return fs.readFileSync(p); } catch (error) { return Buffer.alloc(0); }
  });
  shell.setLoadResourceAsyncCallback((request) => log('loadResourceAsync', JSON.stringify(request)));
  shell.setHttpRequestCallback((request) => log('httpRequest', JSON.stringify(request)));
  const descriptor = shared.setMemory(KEY, frameSize * 4 + 8);
  const bundle = process.env.L5_BUNDLE ||
    path.join(RES, 'node_modules', 'skyline-addon', 'bundle');
  const addon = path.join(RES, 'node_modules', 'skyline-addon', 'build', 'skyline.node');
  const ret = shell.createWindow(WINDOW_ID, bundle, W, H, DPR, true, null, addon, descriptor);
  log('createWindow', ret && ret.constructor && ret.constructor.name);

  let lastSaved = -1;
  let lastMutation = 0;
  let pendingTimers = 0;
  let pollTimer = null;

  function start() {
    if (started) return;
    started = true;
    try {
      shell.notifyAppLaunch(WINDOW_ID, 0, { backgroundColorContent: '#FFFFFFFF' });
      log('notifyAppLaunch');
    } catch (error) {
      log('notifyAppLaunch error', String(error));
    }
    setTimeout(beginOps, 500);
  }

  function createElement(ctx, op) {
    const tag = String(op.tag || 'empty');
    const name = String(op.name || '');
    if (tag === 'text' || name === 'text' || name === 'wx-text') return ctx.createTextNode('');
    if (tag === 'fragment') return ctx.createFragment('fragment');
    if (name === 'image' || name === 'wx-image' || name === 'cover-image') {
      return ctx.createElement('image', name);
    }
    return ctx.createElement(tag, name);
  }

  function setText(node, text) {
    if (typeof node.setText === 'function') { node.setText(String(text)); return true; }
    if (node.ref && typeof node.ref.setText === 'function') { node.ref.setText(String(text)); return true; }
    return false;
  }

  function applyOps(ctx, nodes, ops) {
    for (const op of ops) {
      try {
        switch (op.op) {
          case 'createElement':
            nodes.set(op.id, createElement(ctx, op));
            break;
          case 'setText':
            if (!setText(nodes.get(op.id), op.text)) log('setText unsupported', op.id);
            break;
          case 'setAttribute':
          case 'setAttributeValue':
            nodes.get(op.id).setAttribute(op.name, op.value);
            break;
          case 'setAttributes': {
            const node = nodes.get(op.id);
            if (typeof node.setAttributes === 'function') node.setAttributes(op.attrs || {});
            else for (const [key, value] of Object.entries(op.attrs || {})) node.setAttribute(key, value);
            break;
          }
          case 'removeAttribute':
            nodes.get(op.id).removeAttribute(op.name);
            break;
          case 'setStyle':
            nodes.get(op.id).setStyle(String(op.value));
            break;
          case 'addClass':
            nodes.get(op.id).addClass(String(op.prefix), op.name);
            break;
          case 'removeClass':
            nodes.get(op.id).removeClass(String(op.prefix), op.name);
            break;
          case 'setClass':
            nodes.get(op.id).setClass(String(op.names));
            break;
          case 'setId':
            nodes.get(op.id).setId(String(op.value));
            break;
          case 'appendChild':
            nodes.get(op.id).appendChild(nodes.get(op.child));
            break;
          case 'insertBefore':
            nodes.get(op.id).insertBefore(nodes.get(op.child), nodes.get(op.before));
            break;
          case 'removeChild':
            nodes.get(op.id).removeChild(nodes.get(op.child));
            break;
          case 'replaceChild':
            nodes.get(op.id).replaceChild(nodes.get(op.child), op.index);
            break;
          case 'splice': {
            const items = (op.items || []).map((id) => nodes.get(id));
            nodes.get(op.id).splice(op.pos, op.count, ...items);
            break;
          }
          case 'release':
            nodes.get(op.id).release();
            break;
          case 'appendStyleSheet':
            ctx.appendStyleSheet(String(op.css), op.index);
            break;
          case 'attach': {
            const root = nodes.get(op.root);
            if (op.host !== undefined && op.host !== null) ctx.attach(root, nodes.get(String(op.host)));
            else ctx.attach(root);
            break;
          }
          case 'setScrollTop': {
            const node = nodes.get(op.id);
            let done = false;
            for (const attrs of [{ scrollTop: op.value }, { scroll_top: op.value }]) {
              try { node.setAttributes(attrs); done = true; log('setScrollTop attrs', JSON.stringify(attrs)); break; } catch (error) { log('setScrollTop attrs error', String(error)); }
            }
            if (!done) {
              for (const [name, value] of [['scrollTop', op.value], ['scroll-top', op.value], ['scroll_top', op.value]]) {
                try { node.setAttribute(name, value); done = true; log('setScrollTop attr', name, value); break; } catch (error) { log('setScrollTop attr error', name, String(error)); }
              }
            }
            log('setScrollTop', op.id, op.value, done ? 'ok' : 'failed');
            break;
          }
          case 'startRender':
            ctx.startRender(() => log('render callback'));
            break;
          default:
            // Recording-only / native events are ignored in replay.
            break;
        }
      } catch (error) {
        log('op error', op.op, String(error && error.stack || error));
      }
    }
  }

  function beginOps() {
    if (runStarted) return;
    runStarted = true;
    try {
      const page = new global.SkylineGlobal.PageContext(0, WINDOW_ID);
      log('context', page.getWindowId(), page.clientWidth, page.clientHeight);
      const nodes = new Map();
      const ops = fixture.ops || [];
      log('apply ops', ops.length);
      applyOps(page, nodes, ops);
      const updates = process.env.L5_SKIP_UPDATES === '1' ? [] : (fixture.updates || []);
      pendingTimers = updates.length;
      for (const update of updates) {
        const delay = Math.max(0, Number(update.afterMs) || 0);
        setTimeout(() => {
          log('apply update', delay);
          applyOps(page, nodes, update.ops || []);
          lastMutation = Date.now();
          pendingTimers--;
        }, delay);
      }
      lastMutation = Date.now();
      pollTimer = setInterval(pollFrame, 100);
    } catch (error) {
      log('beginOps error', String(error && error.stack || error));
      finish(false);
    }
  }

  function pollFrame() {
    try {
      const mem = shared.getMemory(KEY);
      if (!mem) return;
      const u8 = new Uint8Array(mem.buffer || mem, mem.byteOffset || 0, mem.byteLength || mem.length);
      const view = new DataView(u8.buffer, u8.byteOffset, 8);
      const id = view.getUint32(0, true);
      const size = view.getUint32(4, true);
      if (size !== frameSize) return;
      const now = Date.now();
      const stable = now - lastMutation > 400;
      if (id !== lastSaved && (stable || now - lastMutation > 2500)) {
        const pixels = Buffer.from(u8.subarray(8, 8 + size));
        fs.writeFileSync(framePath, pixels);
        report.frame = {
          frameId: id,
          frameSize: size,
          sha256: crypto.createHash('sha256').update(pixels).digest('hex'),
          nonWhite: countNonWhite(pixels),
        };
        lastSaved = id;
        log('saved frame', JSON.stringify(report.frame));
      }
      if (lastSaved >= 0 && now - lastMutation > 400) {
        clearInterval(pollTimer);
        const ok = report.frame && report.frame.nonWhite > 0;
        setTimeout(() => { try { shell.destroyWindow(WINDOW_ID); } catch (error) {} }, 50);
        finish(ok);
      }
      if (now - lastMutation > 5000 && lastSaved < 0) {
        clearInterval(pollTimer);
        finish(false);
      }
    } catch (error) {
      log('poll error', String(error && error.stack || error));
    }
  }
} catch (error) {
  log('fatal', String(error && error.stack || error));
  finish(false);
}

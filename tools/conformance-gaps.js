'use strict';

// Conformance gap report against the official Windows Skyline data.
//
//   node tools/conformance-gaps.js [--examples DIR] [--json] [--out FILE]
//                                   [--no-l5] [--engine FILE]
//
// Sections:
//   1. Windows API oracle: which recorded cases are comparable, and which were
//      skipped because the official module needs an engine context.
//   2. L5 pixels: rerender every fixture on the Linux engine and compare it
//      with the official Windows frame in spec/l5-baseline.win32.json.
//   3. Symbol inventory: counts from spec/kernel-symbols.json plus an optional
//      nm check that the built engine exports all 73 FFI lookups.
//   4. Runtime wiring: official Dart callbacks that the native runtime never
//      looks up with FindCallback.  Update/New/Append-style callbacks are
//      handled natively in C++; event/observer/command names are candidates.
//   5. Real examples: CSS properties and builtin WXML tags used by
//      awesome-skyline examples that the runtime has no parser/handler for.
//
// The report is diagnostic: "used by examples but not referenced by the
// runtime" is a candidate gap, not proof that the official engine supports it.
// Use the L5 harness with a probe fixture to decide a specific property.

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { entryFile, readManifest } = require('./lib/runtime-manifest');

const repoRoot = path.resolve(__dirname, '..');
const runtimeDir = path.join(repoRoot, 'native', 'runtime');

// The runtime is split into modules listed in sources.txt (entry TU plus the
// appservice_* replay modules, their internal headers and the shared public
// headers).  Concatenate them so the scanner sees the same declarations as the
// compiler.
function readRuntimeSource() {
  try {
    const entries = readManifest();
    if (!entries.length) return null;
    return entries.map((entry) => fs.readFileSync(entryFile(entry), 'utf8')).join('\n');
  } catch {
    return null;
  }
}
const symbolsPath = path.join(repoRoot, 'spec', 'kernel-symbols.json');
const oraclePath = path.join(repoRoot, 'spec', 'skyline-node.win32.json');
const l5BasePath = path.join(repoRoot, 'spec', 'l5-baseline.win32.json');

const BUILTIN_TAGS = new Set([
  'view', 'scroll-view', 'swiper', 'swiper-item', 'movable-area', 'movable-view',
  'cover-view', 'cover-image', 'icon', 'text', 'rich-text', 'progress', 'button',
  'checkbox', 'checkbox-group', 'form', 'input', 'label', 'picker', 'picker-view',
  'picker-view-column', 'radio', 'radio-group', 'slider', 'switch', 'textarea',
  'navigator', 'image', 'video', 'map', 'canvas', 'web-view', 'ad',
  'official-account', 'open-data', 'editor', 'match-media', 'page-container',
  'root-portal', 'share-element', 'sticky-header', 'sticky-section', 'list-view',
  'grid-view', 'snapshot', 'draggable-sheet', 'open-container',
  'nested-scroll-header', 'nested-scroll-body', 'selection',
]);

// Tags the runtime handles without an explicit name comparison: `view` is the
// default box, `swiper-item` is consumed by its swiper parent, and the
// template/block/slot family never reaches the native node trees.
const IMPLICIT_TAGS = new Set([
  'view', 'text', 'image', 'swiper-item', 'block', 'slot', 'template',
  'import', 'include', 'br', 'span', 'wxs',
]);

// Callback families that are property/materialization setters.  The native
// runtime builds Flutter widgets directly, so these do not have to go through
// the Dart callback table.
const SETTER_PREFIX = /^(New|Append|Insert|Remove|Replace|Update|Set|Mark|Layout|Dispose|Cancel)/;
// Event/observer/command names are much more likely to be user-visible gaps.
const BEHAVIORAL = /(On[A-Z]|Observer|Gesture|Focus|Blur|Snapshot|Dump|Semantics|Selection|Notify|Request|Trigger|Navigate|Relaunch|Shutdown|CollectGarbage|SendMessage|HitTest|Measure|Take|Get[A-Z]|Evaluate|LoadResource|InstallJSEngine|Start|Stop|Report|Trace|UpdatePlatformBrightness|HandleNativeView|ForwardHttp|SceneToImage|YogaTrigger)/;

// D2 probes: map a real-example candidate to the L5 fixture that exercises it.
const PROBE_FOR_CSS = {
  'aspect-ratio': 'appservice-aspect-ratio',
  'vertical-align': 'appservice-vertical-align',
  'text-overflow': 'appservice-text-overflow',
  'white-space': 'appservice-white-space',
};
const PROBE_FOR_TAG = {
  map: 'appservice-map',
};
const NON_PIXEL_CSS = new Set(['will-change']);

function parseArgs(argv) {
  const options = {
    examples: process.env.SKYLINE_EXAMPLES || path.resolve(repoRoot, '..', 'awesome-skyline', 'examples'),
    json: false,
    out: null,
    l5: true,
    engine: null,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--examples') options.examples = path.resolve(argv[++i]);
    else if (arg === '--json') options.json = true;
    else if (arg === '--out') options.out = path.resolve(argv[++i]);
    else if (arg === '--no-l5') options.l5 = false;
    else if (arg === '--engine') options.engine = path.resolve(argv[++i]);
    else if (arg === '-h' || arg === '--help') {
      console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(2, 23).join('\n'));
      process.exit(0);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  return options;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function walk(dir, predicate, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.name === 'node_modules' || entry.name.startsWith('.')) continue;
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, predicate, out);
    else if (predicate(full)) out.push(full);
  }
  return out;
}

// -- 1. Windows API oracle -------------------------------------------------

function windowsOracle() {
  if (!fs.existsSync(oraclePath)) {
    return { available: false, note: 'spec/skyline-node.win32.json missing; run npm run spec:record' };
  }
  const spec = readJson(oraclePath);
  const counts = { ok: 0, skip: 0, crash: 0, timeout: 0, error: 0 };
  const skipped = [];
  const crashes = [];
  for (const [id, entry] of Object.entries(spec.cases || {})) {
    const status = entry.status || 'unknown';
    counts[status] = (counts[status] || 0) + 1;
    if (status === 'skip') skipped.push({ id, reason: entry.reason || '' });
    if (status === 'crash' || status === 'timeout') {
      crashes.push({ id, status, reason: entry.reason || '', exitCode: entry.exitCode ?? null });
    }
  }
  return {
    available: true,
    meta: spec.meta || {},
    total: Object.keys(spec.cases || {}).length,
    counts,
    skipped,
    crashes,
    engineContextSkipped: skipped.filter((entry) => entry.reason === 'engine-context-required'),
  };
}

// -- 2. L5 pixel comparison ------------------------------------------------

function l5Comparison(enabled) {
  if (!fs.existsSync(l5BasePath)) {
    return { available: false, note: 'spec/l5-baseline.win32.json missing; run npm run l5:windows' };
  }
  const baseline = readJson(l5BasePath);
  const result = { available: true, fixtures: [], totals: { exact: 0, tolerance: 0, known: 0, divergent: 0, skipped: 0, error: 0 } };
  if (!enabled) {
    for (const [name, fixture] of Object.entries(baseline.fixtures || {})) {
      result.fixtures.push({
        name,
        status: fixture.compare === false ? 'skipped' : 'not-compared',
        note: fixture.note || '',
        maxDelta: fixture.maxDelta ?? null,
        mismatchRatio: fixture.mismatchRatio ?? null,
      });
    }
    return result;
  }
  const l5 = require('./l5-baseline');
  const { compareWithWindows, windowsFrame } = require('../test/lib/l5');
  for (const [name, expected] of Object.entries(baseline.fixtures || {})) {
    const entry = {
      name,
      note: expected.note || '',
      maxDelta: expected.maxDelta ?? null,
      mismatchRatio: expected.mismatchRatio ?? null,
    };
    if (expected.compare === false) {
      entry.status = 'skipped';
      result.totals.skipped++;
      result.fixtures.push(entry);
      continue;
    }
    const fixturePath = path.join(repoRoot, 'test', 'fixtures', `${name}.ops.json`);
    if (!fs.existsSync(fixturePath)) {
      entry.status = 'error';
      entry.error = 'fixture missing';
      result.totals.error++;
      result.fixtures.push(entry);
      continue;
    }
    try {
      const linux = l5.render(fixturePath, {
        dpr: baseline.dpr || 2,
        width: baseline.width,
        height: baseline.height,
      });
      const win = windowsFrame(expected, l5BasePath);
      const stats = compareWithWindows(baseline, expected, win, linux);
      entry.maxDelta = stats.maxDelta;
      entry.mismatchRatio = Number(stats.mismatchRatio.toFixed(6));
      entry.passed = stats.passed;
      const known = !!expected.note || stats.maxDelta > (Number.isInteger(expected.tolerance) ? expected.tolerance : baseline.tolerance);
      if (stats.maxDelta === 0) { entry.status = 'exact'; result.totals.exact++; }
      else if (stats.passed && known) { entry.status = 'known-divergence'; result.totals.known++; }
      else if (stats.passed) { entry.status = 'tolerance'; result.totals.tolerance++; }
      else { entry.status = 'divergent'; result.totals.divergent++; }
    } catch (error) {
      entry.status = 'error';
      entry.error = String(error && error.message || error);
      result.totals.error++;
    }
    result.fixtures.push(entry);
  }
  return result;
}

// -- 3. Symbol inventory ---------------------------------------------------

function symbolInventory(options) {
  if (!fs.existsSync(symbolsPath)) {
    return { available: false, note: 'spec/kernel-symbols.json missing; run npm run spec:symbols' };
  }
  const spec = readJson(symbolsPath);
  const result = {
    available: true,
    lookupSymbols: spec.ffi.lookupSymbols.length,
    registeredCallbacks: spec.ffi.registeredCallbacks.length,
    typedefs: spec.ffi.typedefs.length,
    structs: spec.ffi.structs.length,
    engineNativeSymbols: spec.engineNativeSymbols.length,
    addonNamedExports: typeof spec.addon.namedExports === 'number'
      ? spec.addon.namedExports
      : Object.keys(spec.addon.namedExports || {}).length,
    platformChannels: spec.platformChannels || [],
    engine: null,
  };
  const releaseEngine = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-release', 'libflutter_engine.so');
  const debugEngine = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-debug', 'libflutter_engine.so');
  const engine = options.engine || (fs.existsSync(releaseEngine) ? releaseEngine : debugEngine);
  if (fs.existsSync(engine)) {
    const nm = spawnSync('nm', ['-D', '--defined-only', engine], { encoding: 'utf8' });
    if (nm.status === 0) {
      const exported = new Set(nm.stdout.split('\n').map((line) => line.split(' ').pop()));
      const missing = spec.ffi.lookupSymbols.filter((entry) => !exported.has(entry.name)).map((entry) => entry.name);
      result.engine = { path: engine, exported: spec.ffi.lookupSymbols.length - missing.length, missing };
    } else {
      result.engine = { path: engine, error: (nm.stderr || '').trim() || 'nm failed' };
    }
  }
  return result;
}

// -- 4. Runtime callback wiring -------------------------------------------

function runtimeWiring() {
  const source = readRuntimeSource();
  if (source === null) return { available: false };
  const constants = new Map();
  for (const match of source.matchAll(/constexpr\s+uint32_t\s+(k\w+Hash)\s*=\s*(\d+)u/g)) {
    constants.set(match[1], Number(match[2]));
  }
  const used = new Set();
  for (const match of source.matchAll(/FindCallback\([^,]+,\s*(k\w+Hash)\s*\)/g)) {
    used.add(match[1]);
  }
  const spec = fs.existsSync(symbolsPath) ? readJson(symbolsPath) : null;
  const callbacks = spec ? spec.ffi.registeredCallbacks : [];
  const byHash = new Map(callbacks.map((entry) => [entry.hash, entry.name]));
  const invoked = [];
  const unknown = [];
  for (const constant of used) {
    const hash = constants.get(constant);
    if (byHash.has(hash)) invoked.push(byHash.get(hash));
    else unknown.push({ constant, hash });
  }
  const notInvoked = callbacks
    .filter((entry) => !invoked.includes(entry.name))
    .map((entry) => entry.name);
  const candidates = notInvoked.filter((name) => !SETTER_PREFIX.test(name) && BEHAVIORAL.test(name));
  const byPrefix = {};
  for (const name of notInvoked) {
    const prefix = name.match(/^[A-Z][a-z]*/)?.[0] || name;
    byPrefix[prefix] = (byPrefix[prefix] || 0) + 1;
  }
  return {
    available: true,
    hashConstants: constants.size,
    invoked: invoked.sort(),
    invokedCount: invoked.length,
    unknownHashes: unknown,
    registered: callbacks.length,
    notInvokedCount: notInvoked.length,
    candidates: candidates.sort(),
    byPrefix,
  };
}

// -- 5. Real example coverage ---------------------------------------------

function runtimeCssKeys() {
  const source = readRuntimeSource();
  if (source === null) return new Set();
  const keys = new Set();
  for (const match of source.matchAll(/\bkey\s*==\s*"([^"]+)"/g)) keys.add(match[1]);
  return keys;
}

function runtimeTags() {
  const source = readRuntimeSource();
  if (source === null) return new Set();
  const tags = new Set();
  for (const match of source.matchAll(/\b(?:node->name|node->tag|name|tag)\s*==\s*"([^"]+)"/g)) {
    tags.add(match[1].toLowerCase());
  }
  return tags;
}

function exampleCoverage(examplesDir) {
  if (!fs.existsSync(examplesDir)) {
    return { available: false, note: `examples directory missing: ${examplesDir}` };
  }
  const files = walk(examplesDir, (file) => /\.(wxss|wxml)$/.test(file));
  const css = new Map();
  const tags = new Map();
  for (const file of files) {
    const relative = path.relative(examplesDir, file);
    if (file.endsWith('.wxss')) {
      let text = fs.readFileSync(file, 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
      text = text.replace(/@[a-zA-Z-]+[^{;]*\{/g, '{').replace(/@[a-zA-Z-]+[^;{}]*;/g, '');
      for (const block of text.matchAll(/\{([^{}]*)\}/g)) {
        for (const declaration of block[1].split(';')) {
          const colon = declaration.indexOf(':');
          if (colon < 0) continue;
          const key = declaration.slice(0, colon).trim().toLowerCase();
          const value = declaration.slice(colon + 1).trim();
          if (!value || !/^[a-z-]+$/.test(key) || key.startsWith('--')) continue;
          if (!css.has(key)) css.set(key, new Set());
          css.get(key).add(relative);
        }
      }
    } else {
      const text = fs.readFileSync(file, 'utf8').replace(/<!--[\s\S]*?-->/g, '');
      for (const match of text.matchAll(/<\s*([a-zA-Z][\w-]*)/g)) {
        const tag = match[1].toLowerCase();
        if (!tags.has(tag)) tags.set(tag, new Set());
        tags.get(tag).add(relative);
      }
    }
  }
  const handledCss = runtimeCssKeys();
  const handledTags = runtimeTags();
  const normalizedHandledTags = new Set([...handledTags].map((tag) => tag.replace(/-/g, '')));
  const missingCss = [...css.keys()].filter((key) => !handledCss.has(key)).sort();
  const builtinTags = [...tags.keys()].filter((tag) => BUILTIN_TAGS.has(tag)).sort();
  const missingTags = builtinTags.filter((tag) =>
    !IMPLICIT_TAGS.has(tag) && !normalizedHandledTags.has(tag.replace(/-/g, '')));
  return {
    available: true,
    examplesDir,
    files: files.length,
    cssProperties: css.size,
    wxmlTags: tags.size,
    builtinTags,
    handledCssKeys: handledCss.size,
    missingCss: missingCss.map((key) => ({ key, files: [...css.get(key)] })),
    missingTags: missingTags.map((tag) => ({ tag, files: [...tags.get(tag)] })),
    customTags: [...tags.keys()].filter((tag) => !BUILTIN_TAGS.has(tag)).sort(),
  };
}

// -- Report ----------------------------------------------------------------

function renderMarkdown(report) {
  const lines = [];
  const l5ByName = new Map((report.l5.fixtures || []).map((fixture) => [fixture.name, fixture]));
  lines.push('# Skyline Linux 对 Windows 原版一致性缺口报告', '');
  lines.push(`生成时间：${new Date().toISOString()}`, '');

  lines.push('## 1. Windows API oracle（L1/L2）', '');
  const oracle = report.oracle;
  if (!oracle.available) {
    lines.push(`不可用：${oracle.note}`, '');
  } else {
    lines.push(`录制用例：${oracle.total}（ok ${oracle.counts.ok} / skip ${oracle.counts.skip} / crash ${oracle.counts.crash || 0} / timeout ${oracle.counts.timeout || 0} / error ${oracle.counts.error || 0}）`, '');
    if (oracle.engineContextSkipped.length) {
      lines.push(`引擎上下文跳过：${oracle.engineContextSkipped.length} 个。这些用例需要 Wine 渲染进程中的官方引擎，当前 ` +
        '`spec:record` 使用 ELECTRON_RUN_AS_NODE，无法录制。', '');
      lines.push('候选缺口（需补录 oracle）：' + oracle.engineContextSkipped.map((entry) => `\`${entry.id}\``).join('、'), '');
    } else {
      lines.push('engine-context 用例已通过 Wine 渲染进程 oracle 补录。' +
        `官方进程崩溃 ${oracle.counts.crash || 0} 个（无稳定基准，契约测试跳过）。`, '');
      if (oracle.crashes && oracle.crashes.length) {
        for (const crash of oracle.crashes) {
          lines.push(`- \`${crash.id}\` — ${crash.reason || crash.status}`);
        }
        lines.push('');
      }
    }
  }

  lines.push('## 2. L5 像素对比（官方 Windows 引擎帧）', '');
  const l5 = report.l5;
  if (!l5.available) {
    lines.push(`不可用：${l5.note}`, '');
  } else {
    const t = l5.totals;
    lines.push(`夹具：${l5.fixtures.length}，逐像素一致 ${t.exact}，容差内 ${t.tolerance}，` +
      `已知差异 ${t.known}，超差 ${t.divergent}，不可比 ${t.skipped}，错误 ${t.error}。`, '');
    const notable = l5.fixtures.filter((fixture) =>
      fixture.status === 'divergent' || fixture.status === 'error' || fixture.status === 'skipped' ||
      fixture.status === 'known-divergence' ||
      (fixture.maxDelta !== null && fixture.maxDelta > 0) || fixture.note);
    if (notable.length) {
      lines.push('| 夹具 | 状态 | maxDelta | 失配率 | 说明 |');
      lines.push('| --- | --- | ---: | ---: | --- |');
      for (const fixture of notable) {
        lines.push(`| ${fixture.name} | ${fixture.status} | ${fixture.maxDelta ?? '-'} | ` +
          `${fixture.mismatchRatio === null || fixture.mismatchRatio === undefined ? '-' : fixture.mismatchRatio} | ` +
          `${(fixture.note || fixture.error || '').replace(/\|/g, '\\|')} |`);
      }
      lines.push('');
    }
  }

  lines.push('## 3. 官方符号清单', '');
  const symbols = report.symbols;
  if (!symbols.available) {
    lines.push(`不可用：${symbols.note}`, '');
  } else {
    lines.push(`FFI lookup ${symbols.lookupSymbols}、注册回调 ${symbols.registeredCallbacks}、` +
      `engine @Native ${symbols.engineNativeSymbols}、addon named exports ${symbols.addonNamedExports}、` +
      `typedef ${symbols.typedefs}、struct ${symbols.structs}。`, '');
    if (symbols.engine) {
      if (symbols.engine.error) lines.push(`引擎符号检查失败：${symbols.engine.error}`, '');
      else {
        lines.push(`引擎导出：${symbols.engine.exported}/${symbols.lookupSymbols}` +
          `${symbols.engine.missing.length ? '，缺失：' + symbols.engine.missing.join('、') : '（全部存在）'}。`, '');
      }
    }
  }

  lines.push('## 4. 原生运行时回调接线', '');
  const wiring = report.wiring;
  if (!wiring.available) {
    lines.push('不可用：缺少 native/runtime/sources.txt 或其列出的运行时源码', '');
  } else {
    lines.push(`C++ 通过 FindCallback 调用 ${wiring.invokedCount}/${wiring.registered} 个官方注册回调；` +
      `其余 ${wiring.notInvokedCount} 个中，New/Update/Append/Set 等属性构造类由 C++ 直接实现，` +
      '以下“事件/观察/命令”类回调是最可能的功能缺口候选：', '');
    const candidates = wiring.candidates;
    if (candidates.length) {
      for (const name of candidates) lines.push(`- \`${name}\``);
      lines.push('');
    } else {
      lines.push('（无候选）', '');
    }
    lines.push('> 注意：候选不等于未实现；部分能力可能由 C++ 直接处理或 Rust/Dart 侧完成。');
    lines.push('> D3 的逐项分类与探针状态见 `docs/callback-coverage.md`。', '');
  }

  lines.push('## 5. 真实示例覆盖（awesome-skyline/examples）', '');
  const examples = report.examples;
  if (!examples.available) {
    lines.push(`不可用：${examples.note}`, '');
  } else {
    lines.push(`扫描 ${examples.files} 个 WXSS/WXML 文件；CSS 属性 ${examples.cssProperties} 个，` +
      `其中运行时没有 "key ==" 分支的 ${examples.missingCss.length} 个；` +
      `内置 WXML 标签 ${examples.builtinTags.length} 个，运行时未识别 ${examples.missingTags.length} 个。`, '');
    if (examples.missingCss.length) {
      lines.push('### 5.1 示例使用但运行时无解析分支的 CSS 属性', '');
      for (const entry of examples.missingCss) {
        let suffix = '';
        const probeName = PROBE_FOR_CSS[entry.key];
        const probe = probeName && l5ByName.get(probeName);
        if (probe) {
          suffix = ` — L5 探针 \`${probeName}\` ${probe.status}` +
            (probe.mismatchRatio ? `，失配率 ${probe.mismatchRatio}` : '');
        } else if (NON_PIXEL_CSS.has(entry.key)) {
          suffix = ' — 纯渲染提示，D3 已确认无行为差异';
        }
        lines.push(`- \`${entry.key}\` — ${entry.files.slice(0, 3).join('、')}` +
          `${entry.files.length > 3 ? ` 等 ${entry.files.length} 个文件` : ''}${suffix}`);
      }
      lines.push('');
    }
    if (examples.missingTags.length) {
      lines.push('### 5.2 示例使用但运行时未识别的内置标签', '');
      for (const entry of examples.missingTags) {
        const probeName = PROBE_FOR_TAG[entry.tag];
        const probe = probeName && l5ByName.get(probeName);
        const suffix = probe
          ? ` — L5 探针 \`${probeName}\` ${probe.status}` +
            (probe.mismatchRatio
              ? `，失配率 ${probe.mismatchRatio}`
              : '（合成 op 流未覆盖原生内容）')
          : '';
        lines.push(`- \`${entry.tag}\` — ${entry.files.slice(0, 3).join('、')}` +
          `${entry.files.length > 3 ? ` 等 ${entry.files.length} 个文件` : ''}${suffix}`);
      }
      lines.push('');
    }
    lines.push('> 自定义组件（如 `album-image`、`navigation-bar`）不在内置标签集合内，未计入缺口。', '');
  }

  lines.push('## 建议的下一步', '');
  lines.push('1. ~~用 Wine 渲染进程补录 24 个 engine-context 契约用例~~（已完成：60 个用例中 50 个可观测、10 个官方崩溃；`test:native` 53/53 通过）。');
  lines.push('2. D2 已完成可视 CSS/标签探针（aspect-ratio/vertical-align/text-overflow/white-space/map）；D6 修复了 aspect-ratio/inline-block/white-space/text-overflow。');
  lines.push('3. D3 已完成 `pointer-events` 探针与候选回调分类（`docs/callback-coverage.md`）；D7/D8/D9 已补齐滚动、Swiper/PickerView/Input、IntersectionObserver/Selection/Refresher/返回拦截；无障碍与 DevTools 自动化仍待 D10。');
  lines.push('4. 录制真实页面的 ops 流，在 Wine 官方引擎和 Linux 引擎上重放并做整页对比。');
  lines.push('');
  return lines.join('\n');
}

function main() {
  let options;
  try {
    options = parseArgs(process.argv.slice(2));
  } catch (error) {
    console.error(String(error.message || error));
    process.exit(2);
  }
  const report = {
    generatedAt: new Date().toISOString(),
    oracle: windowsOracle(),
    l5: l5Comparison(options.l5),
    symbols: symbolInventory(options),
    wiring: runtimeWiring(),
    examples: exampleCoverage(options.examples),
  };
  const text = options.json ? JSON.stringify(report, null, 2) : renderMarkdown(report);
  if (options.out) {
    fs.writeFileSync(options.out, text + '\n');
    console.error(`wrote ${options.out}`);
  } else {
    process.stdout.write(text + '\n');
  }
}

main();

'use strict';

// The embedded runtime is split into `appservice_*` translation units listed in
// `native/runtime/sources.txt`.  CMake, the engine patch script and the
// conformance scanner all consume that manifest, so keep it exhaustive.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { entryFile, nativeDir, readManifest, runtimeDir } = require('../tools/lib/runtime-manifest');

function manifest() {
  const sections = new Map();
  for (const entry of readManifest()) {
    if (!sections.has(entry.section)) sections.set(entry.section, []);
    sections.get(entry.section).push(entry.path);
  }
  return sections;
}

function runtimeFiles() {
  const files = [];
  for (const entry of fs.readdirSync(runtimeDir, { withFileTypes: true })) {
    if (entry.isFile() && /\.(cc|hh)$/.test(entry.name)) files.push(entry.name);
  }
  for (const entry of fs.readdirSync(path.join(runtimeDir, 'internal'), { withFileTypes: true })) {
    if (entry.isFile() && entry.name.endsWith('.hh')) files.push(path.posix.join('internal', entry.name));
  }
  return files.sort();
}

test('runtime manifest: every listed file exists and is listed once', () => {
  const entries = readManifest();
  const all = entries.map(entry => entry.path);
  assert.deepEqual([...new Set(all)].sort(), all.slice().sort(),
    'sources.txt lists a file more than once');
  for (const entry of entries) {
    assert.ok(fs.existsSync(entryFile(entry)), `missing runtime file: [${entry.section}] ${entry.path}`);
  }
});

test('runtime manifest: every runtime source is listed', () => {
  const sections = manifest();
  const listed = new Set([...(sections.get('common') ?? []), ...(sections.get('embedded') ?? [])]);
  for (const file of runtimeFiles()) {
    assert.ok(listed.has(file), `${file} is not listed in sources.txt`);
  }
});

test('runtime manifest: common holds only the ABI entry TU', () => {
  const common = manifest().get('common') ?? [];
  assert.deepEqual(common.filter((file) => file.endsWith('.cc')), ['skyline_runtime_stub.cc']);
  const embedded = manifest().get('embedded') ?? [];
  assert.ok(embedded.filter((file) => file.endsWith('.cc')).length >= 10,
    'the embedded replay should be split into multiple modules');
});

test('runtime manifest: shared files live under native/include or native/common', () => {
  for (const file of manifest().get('shared') ?? []) {
    assert.match(file, /^(include\/skyline|common)\//, `unexpected shared path: ${file}`);
  }
});

// The engine compiles copies of the listed files only, so every shared header
// a runtime file includes must be listed too.
test('runtime manifest: shared headers included by the runtime are listed', () => {
  const entries = readManifest();
  const shared = new Set(entries.filter(entry => entry.section === 'shared').map(entry => entry.path));
  for (const entry of entries) {
    const text = fs.readFileSync(entryFile(entry), 'utf8');
    for (const match of text.matchAll(/^#include "(skyline\/[^"]+)"/gm)) {
      const included = path.posix.join('include', match[1]);
      assert.ok(fs.existsSync(path.join(nativeDir, included)), `${entry.path}: missing ${included}`);
      assert.ok(shared.has(included), `${entry.path} includes ${included}, which is not in [shared]`);
    }
  }
});

test('runtime manifest: every embedded module uses the shared declaration header', () => {
  for (const file of manifest().get('embedded') ?? []) {
    if (!file.endsWith('.cc')) continue;
    const source = fs.readFileSync(path.join(runtimeDir, file), 'utf8');
    assert.ok(source.includes('#include "internal/skyline_runtime_module.hh"'),
      `${file} does not include internal/skyline_runtime_module.hh`);
  }
});

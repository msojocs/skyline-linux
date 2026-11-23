'use strict';

// The embedded runtime is split into `appservice_*` translation units listed in
// `native/runtime/sources.txt`.  CMake, the engine patch script and the
// conformance scanner all consume that manifest, so keep it exhaustive.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..');
const runtimeDir = path.join(repoRoot, 'native', 'runtime');

function manifest() {
  const sections = new Map();
  let section = null;
  for (const raw of fs.readFileSync(path.join(runtimeDir, 'sources.txt'), 'utf8').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const header = /^\[([a-z]+)\]$/.exec(line);
    if (header) {
      section = header[1];
      sections.set(section, []);
      continue;
    }
    assert.ok(section, `source outside a section: ${line}`);
    sections.get(section).push(line);
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
  const sections = manifest();
  const all = [...sections.values()].flat();
  assert.deepEqual([...new Set(all)].sort(), all.slice().sort(),
    'sources.txt lists a file more than once');
  for (const file of all) {
    assert.ok(fs.existsSync(path.join(runtimeDir, file)), `missing runtime file: ${file}`);
  }
});

test('runtime manifest: every runtime source is listed', () => {
  const listed = new Set([...manifest().values()].flat());
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

test('runtime manifest: every embedded module uses the shared declaration header', () => {
  for (const file of manifest().get('embedded') ?? []) {
    if (!file.endsWith('.cc')) continue;
    const source = fs.readFileSync(path.join(runtimeDir, file), 'utf8');
    assert.ok(source.includes('#include "internal/skyline_runtime_module.hh"'),
      `${file} does not include internal/skyline_runtime_module.hh`);
  }
});

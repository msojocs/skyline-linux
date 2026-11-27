'use strict';

// Engine-free checks of the runtime's addon/host-facing C ABI.
//
// native/include/skyline/runtime_api.h is the only declaration of these
// functions: the runtime includes it (so the compiler checks each definition),
// skyline.node and the hosts include it instead of writing their own extern
// "C" declarations, and the engine export list is generated from it.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { entryFile, nativeDir, readManifest } = require('../tools/lib/runtime-manifest');

const repoRoot = path.resolve(__dirname, '..');
const apiHeader = path.join(nativeDir, 'include', 'skyline', 'runtime_api.h');

function apiNames() {
  const text = fs.readFileSync(apiHeader, 'utf8');
  return [...text.matchAll(/^SKYLINE_API\s[^;(]*?\b(\w+)\s*\(/gm)].map(match => match[1]);
}

// name -> number of `SKYLINE_EXPORT extern "C"` definitions in the runtime.
function runtimeExports() {
  const counts = new Map();
  for (const entry of readManifest()) {
    if (!entry.path.endsWith('.cc')) continue;
    const text = fs.readFileSync(entryFile(entry), 'utf8');
    for (const match of text.matchAll(/^SKYLINE_EXPORT extern "C"\s+[\w\s*]*?\b(\w+)\s*\(/gm)) {
      counts.set(match[1], (counts.get(match[1]) || 0) + 1);
    }
  }
  return counts;
}

function sourceFiles(dir) {
  return fs.readdirSync(dir, { withFileTypes: true })
    .filter(entry => entry.isFile() && /\.(cc|hh|h)$/.test(entry.name))
    .map(entry => path.join(dir, entry.name));
}

test('runtime api: every declaration is defined by the runtime', () => {
  const names = apiNames();
  assert.ok(names.length >= 30, `expected the bridge API, found ${names.length} declarations`);
  assert.equal(new Set(names).size, names.length, 'runtime_api.h declares a function twice');
  const exports = runtimeExports();
  for (const name of names) {
    assert.ok(exports.has(name), `${name} is declared in runtime_api.h but not exported by the runtime`);
  }
});

test('runtime api: every non-FFI runtime export is declared', () => {
  const spec = JSON.parse(fs.readFileSync(path.join(repoRoot, 'spec', 'kernel-symbols.json'), 'utf8'));
  const ffi = new Set(spec.ffi.lookupSymbols.map(entry => entry.name));
  const declared = new Set(apiNames());
  const undeclared = [...runtimeExports().keys()].filter(name => !ffi.has(name) && !declared.has(name));
  assert.deepEqual(undeclared, [],
    'runtime exports that neither package:skyline looks up nor runtime_api.h declares');
});

test('runtime api: skyline.node and the hosts do not declare runtime functions', () => {
  const offenders = [];
  for (const dir of ['src', 'host']) {
    for (const file of sourceFiles(path.join(nativeDir, dir))) {
      const text = fs.readFileSync(file, 'utf8');
      if (/\bextern\s+"C"/.test(text)) offenders.push(path.relative(repoRoot, file));
    }
  }
  assert.deepEqual(offenders, [], 'include skyline/runtime_api.h instead of declaring extern "C" functions');
});

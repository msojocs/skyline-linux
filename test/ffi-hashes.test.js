'use strict';

// Engine-free check of the Dart callback hashes hard-coded in the runtime.
//
// The runtime finds package:skyline's registered callbacks by
// `stringHash(name)` (native/runtime/internal/skyline_runtime_ffi.hh and a few
// local constants).  A wrong constant only shows up at run time as a missing
// callback, so compare every `k<Name>Hash` with the registered callbacks
// extracted from the official kernel (spec/kernel-symbols.json).

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..');
const runtimeDir = path.join(repoRoot, 'native', 'runtime');

// Constants whose C++ name differs from the Dart callback name.
const ALIASES = {
  OpacityUpdate: 'OpacityUpdateOpacity',
  YGNodeTransformOrigin: 'YGNodeUpdateTransformOriginAndAlignment',
};

function runtimeSources() {
  const files = [];
  for (const dir of [runtimeDir, path.join(runtimeDir, 'internal')]) {
    for (const name of fs.readdirSync(dir)) {
      if (/\.(cc|hh)$/.test(name)) files.push(path.join(dir, name));
    }
  }
  return files;
}

function hashConstants() {
  const constants = [];
  const pattern = /constexpr\s+uint32_t\s+k(\w+)Hash\s*=\s*(\d+)u?\s*;/g;
  for (const file of runtimeSources()) {
    const text = fs.readFileSync(file, 'utf8');
    for (const match of text.matchAll(pattern)) {
      constants.push({ name: match[1], value: Number(match[2]), file: path.relative(repoRoot, file) });
    }
  }
  return constants;
}

test('ffi hashes: every runtime callback hash matches the official kernel', () => {
  const spec = JSON.parse(fs.readFileSync(path.join(repoRoot, 'spec', 'kernel-symbols.json'), 'utf8'));
  const registered = new Map(spec.ffi.registeredCallbacks.map(entry => [entry.name, entry.hash]));
  const constants = hashConstants();
  assert.ok(constants.length > 100, `expected the runtime hash table, found ${constants.length}`);
  const problems = [];
  for (const { name, value, file } of constants) {
    const dartName = ALIASES[name] || name;
    if (!registered.has(dartName)) {
      problems.push(`${file}: k${name}Hash has no registered callback "${dartName}"`);
    } else if (registered.get(dartName) !== value) {
      problems.push(`${file}: k${name}Hash = ${value}, kernel registers ${dartName} = ${registered.get(dartName)}`);
    }
  }
  assert.deepEqual(problems, []);
});

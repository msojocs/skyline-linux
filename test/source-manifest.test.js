'use strict';

// The embedded bootstrap is assembled from ordered parts (CMake and the tests
// consume the same manifest).  These checks protect the concatenation rules
// that let the parts behave like the original single IIFE.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { bootstrapPartNames, loadBootstrapSource } = require('./lib/bootstrap-source');

const repoRoot = path.resolve(__dirname, '..');
const partsDir = path.join(repoRoot, 'native', 'src', 'bootstrap');

test('bootstrap manifest: every part is listed and exists', () => {
  const names = bootstrapPartNames();
  assert.ok(names.length > 0, 'parts.txt is not empty');
  const listed = new Set(names);
  for (const name of names) {
    assert.ok(fs.existsSync(path.join(partsDir, name)), `missing part: ${name}`);
  }
  const actual = fs.readdirSync(partsDir)
    .filter((name) => name.endsWith('.js'))
    .sort();
  assert.deepEqual(actual, [...listed].sort(),
    'every bootstrap *.js is listed in parts.txt exactly once');
});

test('bootstrap manifest: concatenation keeps the single-IIFE invariants', () => {
  const source = loadBootstrapSource();
  assert.ok(source.startsWith("'use strict';"),
    'the first part starts with use strict');
  assert.ok(/^'use strict';\n[\s\S]*?\(\(\) => \{/.test(source),
    'the top-level IIFE opens in the first part');
  assert.ok(/}\)\(\);\s*$/.test(source), 'the last part closes the IIFE');
  assert.equal(source.includes(')SKYLINEJS"'), false,
    'no part contains the bootstrap_js.h raw-string terminator');
  assert.equal(source.includes('require('), false,
    'the bootstrap still has no require() calls');
  assert.equal(source.includes('module.exports'), false,
    'the bootstrap still has no module references');
});

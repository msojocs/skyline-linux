'use strict';

// Engine-free checks of the environment variable registry
// (tools/env-vars.json): the generated reference and native table are up to
// date, every SKYLINE_* variable the code reads is registered, and native code
// reads them only through skyline::config.

const test = require('node:test');
const assert = require('node:assert/strict');
const { loadRegistry, nativeId, registryProblems } = require('../tools/env-doc');

test('env registry: code and generated files agree with tools/env-vars.json', () => {
  assert.deepEqual(registryProblems(loadRegistry()), []);
});

test('env registry: native ids are derived from the names', () => {
  assert.equal(nativeId('SKYLINE_APP_SERVICE_DEBUG'), 'kAppServiceDebug');
  assert.equal(nativeId('SKYLINE_STUB_PAGE_JSON'), 'kStubPageJson');
});

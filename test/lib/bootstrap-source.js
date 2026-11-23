'use strict';

// The embedded bootstrap is split into ordered parts under
// `native/src/bootstrap/`.  It must still execute as a single script (all
// state is closure-local and `native` is captured from the global bridge), so
// CMake and the bootstrap tests both concatenate the manifest in this order.

const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..');
const partsDir = path.join(repoRoot, 'native', 'src', 'bootstrap');

function bootstrapPartNames() {
  return fs.readFileSync(path.join(partsDir, 'parts.txt'), 'utf8')
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean);
}

function loadBootstrapSource() {
  return bootstrapPartNames()
    .map((name) => fs.readFileSync(path.join(partsDir, name), 'utf8'))
    .join('');
}

module.exports = { bootstrapPartNames, loadBootstrapSource };

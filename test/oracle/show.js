'use strict';

const fs = require('node:fs');
const { specPath } = require('../lib/env');

const spec = JSON.parse(fs.readFileSync(specPath(), 'utf8'));
const summary = { total: 0, ok: 0, skip: 0, crash: 0, timeout: 0, error: 0 };
const groups = {};
for (const [id, entry] of Object.entries(spec.cases)) {
  summary.total++;
  summary[entry.status] = (summary[entry.status] || 0) + 1;
  const group = id.split('.')[0];
  groups[group] = groups[group] || { total: 0, ok: 0, skip: 0, crash: 0, timeout: 0, error: 0 };
  groups[group].total++;
  groups[group][entry.status] = (groups[group][entry.status] || 0) + 1;
  if (process.argv.includes('--crashes') && (entry.status === 'crash' || entry.status === 'timeout')) {
    console.log(`${entry.status} ${id} exit=${entry.exitCode}`);
  }
}
console.log(JSON.stringify({ meta: spec.meta, summary, groups }, null, 2));

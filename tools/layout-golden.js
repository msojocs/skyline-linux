'use strict';

// Structural layout goldens for the op-replay fixtures.
//
//   node tools/layout-golden.js --write [--only name1,name2]
//   node tools/layout-golden.js [--only ...]        # compare, print diffs
//
// The runtime appends one snapshot per distinct layout state to
// SKYLINE_LAYOUT_DUMP (native/runtime/appservice_model.cc): every node's id,
// parent, tag/name/classes/text, laid-out rect, a summary of the resolved style
// and the widgets BuildWidget materialized.  test/golden/layout/<fixture>.json
// keeps the whole sequence for every fixture in test/fixtures, so runtime
// refactors are checked node by node instead of through pixels
// (native/test/layout-golden.test.js).

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { SKYLINE_DEBUG, missingPrerequisites, paths, runHostAsync } = require('../test/lib/host');
const { buildProblems } = require('./build-status');

const repoRoot = path.resolve(__dirname, '..');
const goldenDir = path.join(repoRoot, 'test', 'golden', 'layout');
const fixturesDir = path.join(repoRoot, 'test', 'fixtures');
const resultsDir = path.join(repoRoot, 'test', 'results', 'layout');

// Long enough for the latest fixture update batch (900 ms) and its transitions.
const TIMEOUT_MS = 2500;
// Concurrent host replays; npm test already runs other host test files beside
// this one (tools/run-tests.js).
const CONCURRENCY = Math.max(1, Math.min(8, Math.floor(os.availableParallelism() / 4)));

function fixtureNames() {
  return fs.readdirSync(fixturesDir)
    .filter(name => name.endsWith('.ops.json'))
    .map(name => name.replace(/\.ops\.json$/, ''))
    .sort();
}

function goldenPath(name) {
  return path.join(goldenDir, `${name}.json`);
}

// Replays `name` and returns its snapshot sequence (arrays of nodes).
async function capture(name) {
  const dump = path.join(os.tmpdir(),
    `skyline-layout-${process.pid}-${Math.random().toString(36).slice(2, 8)}.jsonl`);
  try {
    const { result, logs } = await runHostAsync({
      key: 'skyline_layout',
      ops: paths.fixture(name),
      timeoutMs: TIMEOUT_MS,
      frames: 0,
      env: { SKYLINE_LAYOUT_DUMP: dump },
    });
    if (result.status !== 0) throw new Error(`skyline_host failed for ${name} (status ${result.status}):\n${logs}`);
    if (!fs.existsSync(dump)) throw new Error(`no layout snapshot for ${name}:\n${logs}`);
    return fs.readFileSync(dump, 'utf8').split('\n').filter(Boolean).map(line => JSON.parse(line).nodes);
  } finally {
    fs.rmSync(dump, { force: true });
  }
}

// One node per line so golden diffs point at the node that changed.
function serialize(name, snapshots) {
  const blocks = snapshots.map(nodes =>
    `    [\n${nodes.map(node => `      ${JSON.stringify(node)}`).join(',\n')}\n    ]`);
  return `{\n  "fixture": ${JSON.stringify(name)},\n  "snapshots": [\n${blocks.join(',\n')}\n  ]\n}\n`;
}

function readGolden(name) {
  const file = goldenPath(name);
  return fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
}

// Human-readable difference between two serialized goldens (first lines that
// differ, with their snapshot/node context); empty when they are equal.
function describeDiff(expected, actual, limit = 8) {
  if (expected === actual) return '';
  const a = expected.split('\n');
  const b = actual.split('\n');
  const lines = [];
  for (let i = 0; i < Math.max(a.length, b.length) && lines.length < limit * 2; i++) {
    if (a[i] === b[i]) continue;
    lines.push(`- ${(a[i] ?? '<missing>').trim()}`);
    lines.push(`+ ${(b[i] ?? '<missing>').trim()}`);
  }
  return lines.join('\n');
}

// Golden files without a fixture, and fixtures without a golden.
function goldenSetProblems() {
  const fixtures = new Set(fixtureNames());
  const goldens = fs.existsSync(goldenDir)
    ? fs.readdirSync(goldenDir).filter(name => name.endsWith('.json')).map(name => name.slice(0, -5))
    : [];
  const problems = [];
  for (const name of fixtures) {
    if (!goldens.includes(name)) problems.push(`${name}: no golden (run npm run golden:update -- --only ${name})`);
  }
  for (const name of goldens) {
    if (!fixtures.has(name)) problems.push(`${name}: golden without a fixture in test/fixtures`);
  }
  return problems;
}

async function pool(items, worker) {
  const results = new Array(items.length);
  let next = 0;
  await Promise.all(Array.from({ length: Math.min(CONCURRENCY, items.length) }, async () => {
    while (next < items.length) {
      const index = next++;
      results[index] = await worker(items[index]);
    }
  }));
  return results;
}

function parseOnly(argv) {
  const index = argv.indexOf('--only');
  const inline = argv.find(arg => arg.startsWith('--only='));
  const raw = inline ? inline.slice('--only='.length) : index >= 0 ? argv[index + 1] : null;
  return raw ? raw.split(',').map(name => name.trim().replace(/\.ops\.json$/, '')).filter(Boolean) : null;
}

async function main(argv) {
  const missing = missingPrerequisites(SKYLINE_DEBUG);
  if (missing.length) {
    console.error(`missing prerequisites: ${missing.join('; ')}`);
    return 77;
  }
  const write = argv.includes('--write');
  const stale = buildProblems(['engine-debug', 'kernel']);
  if (write && stale.length && !argv.includes('--allow-stale')) {
    console.error(`refusing to write goldens from stale artifacts (--allow-stale overrides):\n  ${stale.join('\n  ')}`);
    return 1;
  }
  const only = parseOnly(argv);
  const names = only || fixtureNames();
  const unknown = names.filter(name => !fixtureNames().includes(name));
  if (unknown.length) {
    console.error(`unknown fixtures: ${unknown.join(', ')}`);
    return 1;
  }
  let failed = 0;
  await pool(names, async name => {
    const actual = serialize(name, await capture(name));
    if (write) {
      // Record only states that replay identically twice.
      const again = serialize(name, await capture(name));
      if (again !== actual) {
        console.error(`FAIL ${name}: layout differs between two replays\n${describeDiff(actual, again)}`);
        failed++;
        return;
      }
      fs.mkdirSync(goldenDir, { recursive: true });
      fs.writeFileSync(goldenPath(name), actual);
      console.error(`wrote ${path.relative(repoRoot, goldenPath(name))}`);
      return;
    }
    const diff = describeDiff(readGolden(name) ?? '', actual);
    if (diff) {
      failed++;
      fs.mkdirSync(resultsDir, { recursive: true });
      fs.writeFileSync(path.join(resultsDir, `${name}.json`), actual);
      console.log(`FAIL ${name}\n${diff}`);
    } else {
      console.log(`ok   ${name}`);
    }
  });
  if (!write) {
    for (const problem of goldenSetProblems()) {
      console.log(`FAIL ${problem}`);
      failed++;
    }
  }
  return failed ? 1 : 0;
}

if (require.main === module) {
  main(process.argv.slice(2)).then(code => { process.exitCode = code; }, error => {
    console.error(error.stack || error.message);
    process.exitCode = 2;
  });
}

module.exports = {
  CONCURRENCY,
  capture,
  describeDiff,
  fixtureNames,
  goldenDir,
  goldenSetProblems,
  readGolden,
  resultsDir,
  serialize,
};

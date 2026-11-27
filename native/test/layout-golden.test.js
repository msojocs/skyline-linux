'use strict';

// Structural layout goldens: every fixture in test/fixtures is replayed and
// its layout snapshots (node rects, resolved style summary, materialized
// widgets; see tools/layout-golden.js) must match test/golden/layout exactly.
// Refactors of the runtime keep these byte-identical; intended changes are
// recorded with `npm run golden:update [-- --only <fixture>]`.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {
  SKYLINE_DEBUG,
  checkBuildCurrent,
  prerequisiteSkip,
} = require('../../test/lib/host');
const golden = require('../../tools/layout-golden');

const skip = prerequisiteSkip(SKYLINE_DEBUG);

test('layout golden: every fixture has a golden and every golden a fixture', () => {
  assert.deepEqual(golden.goldenSetProblems(), []);
});

test('layout golden: build artifacts match the sources', { skip }, t => {
  checkBuildCurrent(t, ['engine-debug', 'kernel']);
});

test('layout golden: fixtures replay to the recorded layout', {
  skip,
  concurrency: golden.CONCURRENCY,
  timeout: 600000,
}, async t => {
  await Promise.all(golden.fixtureNames().map(name => t.test(name, async () => {
    const expected = golden.readGolden(name);
    assert.ok(expected !== null, `${name}: no golden (npm run golden:update -- --only ${name})`);
    const actual = golden.serialize(name, await golden.capture(name));
    const diff = golden.describeDiff(expected, actual);
    if (diff) {
      fs.mkdirSync(golden.resultsDir, { recursive: true });
      const file = path.join(golden.resultsDir, `${name}.json`);
      fs.writeFileSync(file, actual);
      assert.fail(`${name}: layout changed (actual written to ${path.relative(process.cwd(), file)}; ` +
        `npm run golden:update -- --only ${name} records it if intended)\n${diff}`);
    }
  })));
});

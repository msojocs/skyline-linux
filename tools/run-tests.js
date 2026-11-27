'use strict';

// Runs `node --test` with a concurrency suited to the host tests.
//
//   node tools/run-tests.js <test files...>
//
// Every test file that launches skyline_host runs a software-rendering Flutter
// engine with several threads, and some assertions depend on timing (update
// batches, pointer scripts, the L5 capture deadline).  node --test defaults to
// one file per CPU, which oversubscribes the machine and makes those tests
// fail spuriously, so run half as many files at once.
// SKYLINE_TEST_CONCURRENCY overrides the number.

const os = require('node:os');
const { spawnSync } = require('node:child_process');

const concurrency = Number(process.env.SKYLINE_TEST_CONCURRENCY) ||
  Math.max(1, Math.floor(os.availableParallelism() / 2));
const result = spawnSync(process.execPath,
  ['--test', `--test-concurrency=${concurrency}`, ...process.argv.slice(2)],
  { stdio: 'inherit' });
process.exitCode = result.status ?? 1;

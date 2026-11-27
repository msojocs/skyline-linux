'use strict';

// Helpers for tests and tools that launch `skyline_host` with the
// `package:sample/devtools.dart` entrypoint.

const { buildProblems } = require('../../tools/build-status');

/**
 * `--entrypoint-arg` pairs for devtools.dart `main(args)`:
 * window id, runtime holder pointer, width, height, createRuntime and
 * skylineAddonPath.
 *
 * `skylineLibrary` is passed as skylineAddonPath.  package:skyline opens it by
 * path on Linux, exactly as the DevTools opens skyline.node, so it must be a
 * library that exports the Skyline ABI: the engine with the embedded runtime,
 * or the standalone stub `native/build/libskyline.so`.
 */
function devtoolsEntrypointArgs({
  width,
  height,
  skylineLibrary,
  windowId = 1,
  runtimeHolder = 1,
  createRuntime = true,
}) {
  if (!skylineLibrary) {
    throw new Error('devtoolsEntrypointArgs: skylineLibrary is required');
  }
  return [windowId, runtimeHolder, width, height, createRuntime ? 1 : 0, skylineLibrary]
    .flatMap(value => ['--entrypoint-arg', String(value)]);
}

/**
 * Fails the test `t` when the given build steps (tools/build-status.js, e.g.
 * `['engine-debug', 'kernel']`) are stale: host tests would otherwise exercise
 * an old embedded runtime or Dart snapshot and pass or fail for the wrong
 * reason.  `SKYLINE_ALLOW_STALE_BUILD=1` turns the failure into a skip.
 */
function checkBuildCurrent(t, steps) {
  const problems = buildProblems(steps);
  if (problems.length === 0) return;
  if (process.env.SKYLINE_ALLOW_STALE_BUILD === '1') {
    t.skip(`stale build allowed by SKYLINE_ALLOW_STALE_BUILD: ${problems.join('; ')}`);
    return;
  }
  throw new Error(
    'build artifacts are stale (set SKYLINE_ALLOW_STALE_BUILD=1 to run anyway):\n  ' +
    problems.join('\n  '));
}

module.exports = { checkBuildCurrent, devtoolsEntrypointArgs };

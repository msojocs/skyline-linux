'use strict';

// Helpers for tests and tools that launch `skyline_host` with the
// `package:sample/devtools.dart` entrypoint.

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

module.exports = { devtoolsEntrypointArgs };

'use strict';

// Phase-3 appservice smoke test: boot the recompiled `package:skyline` kernel
// against the standalone stub `libskyline.so` runtime and capture the first
// Flutter frame.  This exercises the real `initSkylineForDevtools` path:
//
//   devtools.dart main -> SkylineApplicationDevtools -> openSkylineDylib
//   -> InitDartAPIDLSkyline -> 73 symbol lookups -> setupFFIFunctions(Register)
//   -> ShareRuntimeFromRuntimeShell / CreateWindow -> runApp -> first frame
//
// Skips when the engine, host, stub runtime or recompiled kernel is missing.
// Set SKYLINE_REQUIRE_ENGINE=1 to fail instead of skipping in CI.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const {
  SKYLINE_DEBUG,
  checkBuildCurrent,
  paths,
  prerequisiteSkip,
  runHost,
} = require('../../test/lib/host');

const runtime = paths.stubRuntime;
const width = 320;
const height = 240;
const skip = prerequisiteSkip([...SKYLINE_DEBUG, 'stub-runtime']);

test('appservice: build artifacts match the sources', { skip }, t => {
  checkBuildCurrent(t, ['engine-debug', 'kernel']);
});

test('appservice: stub runtime exports all 73 addon lookup symbols', {
  skip: prerequisiteSkip(['stub-runtime']),
}, () => {
  const spec = JSON.parse(
    fs.readFileSync(path.join(paths.repoRoot, 'spec', 'kernel-symbols.json'), 'utf8'),
  );
  const want = new Set(spec.ffi.lookupSymbols.map(entry => entry.name));
  const nm = spawnSync('nm', ['-D', '--defined-only', runtime], { encoding: 'utf8' });
  assert.equal(nm.status, 0, nm.stderr);
  const have = new Set(
    nm.stdout
      .split('\n')
      .map(line => line.trim())
      .filter(Boolean)
      .map(line => line.split(/\s+/).pop()),
  );
  const missing = [...want].filter(name => !have.has(name)).sort();
  assert.deepEqual(missing, [], `missing exports: ${missing.join(', ')}`);
});

test('appservice: recompiled kernel boots against the stub skyline runtime', {
  skip,
  timeout: 60000,
}, () => {
  // package:skyline opens the stub by path, exactly like the DevTools opens
  // skyline.node; LD_LIBRARY_PATH keeps the legacy by-name lookup on the stub.
  const { result, logs, details, pixels } = runHost({
    key: 'skyline_appservice',
    width,
    height,
    skylineLibrary: runtime,
    libraryPath: path.dirname(runtime),
  });

  assert.equal(result.status, 0, `skyline_host failed (status ${result.status}):\n${logs}`);
  assert.ok(details, `missing SKYLINE_HOST_RESULT:\n${logs}`);
  assert.equal(details.width, width);
  assert.equal(details.height, height);
  assert.equal(details.frameSize, width * height * 4);
  assert.ok(details.vsyncCount >= 1, 'the app never requested a vsync');
  assert.ok(details.presentCount >= 1, 'no frame was presented');

  // The stub runtime was actually dlopen'ed and initialized.
  assert.match(result.stderr, /\[skyline-runtime\] Dart_InitializeApiDL -> 0/);
  assert.match(result.stderr, /\[skyline-runtime:I\]/);
  assert.match(result.stderr, /NotifyFlutterReady -> RunApplication/);
  assert.doesNotMatch(result.stderr, /Failed to load dynamic library/);

  assert.ok(pixels, 'no frame was captured');
  // The splash page without a cached image is a white surface.
  assert.deepEqual([...pixels.subarray(0, 4)], [255, 255, 255, 255], 'first pixel must be white');
});

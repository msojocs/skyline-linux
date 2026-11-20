'use strict';

// Phase-2 AOT through the N-API module: `skyline.node` built against the
// release engine, bundle containing `libapp.so` and no `kernel_blob.bin`.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..');
const addonPath = path.join(repoRoot, 'native', 'build-release', 'skyline.node');
const stage = path.join(repoRoot, 'native', 'engine', 'out', 'linux-x64-release');
const engineLib = path.join(stage, 'libflutter_engine.so');
const aot = path.join(repoRoot, 'apps', 'empty-page', 'build', 'libapp.so');

const width = 128;
const height = 96;
const frameSize = width * height * 4;

function prerequisiteSkip() {
  const missing = [];
  if (!fs.existsSync(addonPath)) {
    missing.push('native/build-release/skyline.node (release module build)');
  }
  if (!fs.existsSync(engineLib)) missing.push('release libflutter_engine.so');
  if (!fs.existsSync(aot)) missing.push('apps/empty-page/build/libapp.so (npm run app:aot)');
  return missing.length ? missing.join('; ') : null;
}

test('host engine aot: N-API createWindow loads libapp.so', {
  skip: prerequisiteSkip() || false,
  timeout: 60000,
}, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'skyline-aot-bundle-'));
  const bundle = path.join(dir, 'bundle');
  fs.mkdirSync(path.join(bundle, 'flutter_assets'), { recursive: true });
  fs.copyFileSync(aot, path.join(bundle, 'libapp.so'));

  require(addonPath);
  const shell = new global.SkylineShell();
  const frame = Buffer.alloc(8 + frameSize * 4);
  let created = false;
  try {
    shell.createWindow(1, bundle, width, height, 1, true, frame, addonPath,
      new Uint8Array(16));
    created = true;
    const result = await new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('timed out waiting for an AOT frame')), 15000);
      shell.onWindowRasterized(1, () => {
        clearTimeout(timer);
        resolve({
          frameId: frame.readUInt32LE(0),
          size: frame.readUInt32LE(4),
          pixel: [...frame.subarray(8, 12)],
        });
      });
    });
    assert.ok(result.frameId >= 1);
    assert.equal(result.size, frameSize);
    assert.deepEqual(result.pixel, [255, 0, 0, 255]);
  } finally {
    if (created) shell.destroyWindow(1);
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

'use strict';

const path = require('node:path');
const fs = require('node:fs');

const repoRoot = path.resolve(__dirname, '..', '..');

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) return candidate;
  }
  return null;
}

function linuxSharedMemoryDefault() {
  return firstExisting([
    process.env.SKYLINE_LINUX_SHARED_MEMORY,
    // Built in-repo by `npm run native:build` / `native:build:release`.
    path.resolve(repoRoot, 'native', 'build', 'sharedMemory.node'),
    path.resolve(repoRoot, 'native', 'build-release', 'sharedMemory.node'),
    // Legacy sibling checkout, kept for existing developer setups.
    path.resolve(repoRoot, '../skyline-shared-memory/build/Release/sharedMemory.node'),
    path.resolve(repoRoot, '../skyline-shared-memory/build/sharedMemory.node'),
  ]);
}

// Sibling Linux WeChat DevTools checkout.  SKYLINE_DEVTOOLS_DIR overrides it,
// matching native/engine/stage-devtools-addon.sh.
function devtoolsDir() {
  return process.env.SKYLINE_DEVTOOLS_DIR || path.resolve(repoRoot, '..', 'wechat-web-devtools-linux');
}

// Reads KEY=VALUE from native/engine/REVISION (pinned versions).
function revisionValue(key) {
  const text = fs.readFileSync(path.join(repoRoot, 'native', 'engine', 'REVISION'), 'utf8');
  const match = new RegExp(`^${key}=(.*)$`, 'm').exec(text);
  return match ? match[1].trim() : null;
}

// Official Windows `resources/app.asar.unpacked` trees, most specific first.
function windowsResourceCandidates() {
  return [
    process.env.SKYLINE_WIN_RESOURCES,
    path.join(devtoolsDir(), 'cache', 'devtools', 'resources', 'app.asar.unpacked'),
    path.join(devtoolsDir(), 'tmp', 'official-win-resources', 'resources', 'app.asar.unpacked'),
  ].filter(Boolean);
}

function windowsResources() {
  const candidates = windowsResourceCandidates();
  // Prefer a tree that contains the Flutter bundle, which the oracle recorder
  // needs; some extracted snapshots only contain the .node modules.
  for (const candidate of candidates) {
    if (fs.existsSync(path.join(candidate, 'node_modules', 'skyline-addon', 'bundle'))) return candidate;
  }
  return firstExisting(candidates);
}

function windowsElectron() {
  return firstExisting([
    process.env.SKYLINE_WIN_ELECTRON,
    path.resolve(repoRoot, '../skyline-client-server/cache/electron-win32-x64/electron.exe'),
  ]);
}

function wineBinary() {
  return process.env.SKYLINE_WINE || 'wine';
}

// Wine exposes the Unix root as Z:.  Convert an absolute Unix path into the
// Windows form so Electron's Node layer resolves the same file.
function toWindowsPath(unixPath) {
  if (!unixPath) return unixPath;
  const absolute = path.resolve(unixPath);
  if (process.platform === 'win32') return absolute;
  return 'Z:' + absolute.replace(/\//g, '\\');
}

function oraclePaths() {
  const resources = windowsResources();
  const electron = windowsElectron();
  if (!resources || !electron) return null;
  return {
    resources,
    electron,
    addon: path.join(resources, 'node_modules', 'skyline-addon', 'build', 'skyline.node'),
    shared: path.join(resources, 'node_modules', 'sharedMemory', 'sharedMemory.node'),
    bundle: path.join(resources, 'node_modules', 'skyline-addon', 'bundle'),
  };
}

function resultsDir() {
  const dir = process.env.SKYLINE_RESULTS_DIR || path.resolve(repoRoot, 'tmp', 'results');
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

function specPath() {
  return process.env.SKYLINE_SPEC || path.resolve(repoRoot, 'spec', 'skyline-node.win32.json');
}

module.exports = {
  repoRoot,
  firstExisting,
  devtoolsDir,
  revisionValue,
  linuxSharedMemoryDefault,
  windowsResourceCandidates,
  windowsResources,
  windowsElectron,
  wineBinary,
  toWindowsPath,
  oraclePaths,
  resultsDir,
  specPath,
};

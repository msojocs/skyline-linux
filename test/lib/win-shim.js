'use strict';

// Builds a Wine-visible Electron install whose `resources/app` points at a
// local app directory.  The official Windows Electron shipped with the
// DevTools client is a packaged app and ignores a CLI app path, so both the L5
// collector and the engine-context oracle run through this shim.

const fs = require('node:fs');
const path = require('node:path');

function ensureShim(electron, appDir, shimDir) {
  const source = path.dirname(electron);
  const appLink = path.join(shimDir, 'resources', 'app');
  let current = null;
  try { current = fs.realpathSync(appLink); } catch {}
  if (fs.existsSync(path.join(shimDir, path.basename(electron))) &&
      current === fs.realpathSync(appDir)) {
    return path.join(shimDir, path.basename(electron));
  }
  fs.rmSync(shimDir, { recursive: true, force: true });
  fs.mkdirSync(path.join(shimDir, 'resources'), { recursive: true });
  for (const entry of fs.readdirSync(source)) {
    if (entry === 'resources') continue;
    const from = path.join(source, entry);
    const to = path.join(shimDir, entry);
    try { fs.linkSync(from, to); } catch { fs.cpSync(from, to, { recursive: true }); }
  }
  fs.symlinkSync(appDir, appLink);
  return path.join(shimDir, path.basename(electron));
}

module.exports = { ensureShim };

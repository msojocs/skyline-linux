'use strict';

// native/runtime/sources.txt: the files compiled into the embedded runtime
// (engine) and the standalone libskyline.so.  Shared by the build-status
// fingerprint, the conformance scanner and the manifest test; CMake and
// native/engine/patch-skyline-runtime.sh parse the same format.
//
//   [common]    native/runtime/<path>, both runtime builds
//   [embedded]  native/runtime/<path>, engine build only
//   [shared]    native/<path>, files the runtime shares with skyline.node and
//               the hosts (public headers, logging, configuration)
//
// The engine copy (skyline_runtime/ in the embedder directory) keeps every
// entry at <copy root>/<path>.

const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..');
const nativeDir = path.join(repoRoot, 'native');
const runtimeDir = path.join(nativeDir, 'runtime');
const manifestPath = path.join(runtimeDir, 'sources.txt');

const SECTIONS = ['common', 'embedded', 'shared'];

// [{ section, path }] in manifest order.
function readManifest(file = manifestPath) {
  const entries = [];
  let section = null;
  for (const raw of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const header = /^\[([a-z]+)\]$/.exec(line);
    if (header) {
      section = header[1];
      if (!SECTIONS.includes(section)) {
        throw new Error(`${file}: unknown section [${section}]`);
      }
      continue;
    }
    if (!section) throw new Error(`${file}: entry outside a section: ${line}`);
    entries.push({ section, path: line });
  }
  return entries;
}

// Absolute path of a manifest entry: in the repository, or under an engine
// copy root when `copyRoot` is given.
function entryFile(entry, copyRoot = null) {
  if (copyRoot) return path.join(copyRoot, entry.path);
  return path.join(entry.section === 'shared' ? nativeDir : runtimeDir, entry.path);
}

module.exports = {
  SECTIONS,
  entryFile,
  manifestPath,
  nativeDir,
  readManifest,
  runtimeDir,
};

'use strict';

// Extracts the wxlib base stylesheets (WASkylineStyle) shipped with the
// WeChat DevTools.
//
// The stylesheet is not a loose file: the DevTools packs its vendor libraries
// into `resources/app.asar` → `js/vendor/<version>.wxvpkg`, and the wxvpkg
// archive contains:
//
//   /WAVersion.json             vendor package version (e.g. 2.25.3)
//   /WASkylineStyle.css.js      __webviewEngine__.setGlobalStyle(..., css)
//   /WASkylineStyle.fpcssb      compiled binary form of the same styles
//   /WASkylineStyleV2.css.js    newer wxlib component styles
//   /WASkylineStyleV2.fpcssb
//   /WAStyleIndex.fpiib         index mapping wxlibfile://WASkylineStyle*
//
// The vendor package is versioned independently from the DevTools app, so the
// version is part of every output path and the manifest:
//
//   tmp/wxlib-style/2.25.3/manifest.json
//   tmp/wxlib-style/2.25.3/WASkylineStyle.css
//   tmp/wxlib-style/2.25.3/WASkylineStyleV2.css
//
// Different wxlib versions can carry different builtin-component styles, so
// consumers must pin the version and record it alongside any rendering
// baseline taken with those styles.  `--vendor` defaults to WXLIB_VERSION in
// native/engine/REVISION.  `--list` prints every vendor package bundled in the
// DevTools with its WAVersion.json metadata.
//
// Usage:
//   node tools/extract-wxlib-style.js [--resources <devtools/resources>]
//                                     [--out <dir>] [--vendor <version>]
//                                     [--list] [--quiet]
//
// Formats (verified against DevTools 2.02.2609250, vendor 2.25.3):
//   ASAR:  [u32 le 4][u32 le headerSize][u32 le jsonLen][padding]JSON
//          first JSON byte is found by scanning for '{' from offset 8; file
//          offsets in the JSON are relative to the 4-aligned end of the JSON.
//   WXVPKG: [16-byte header][u16 be entryCount][entries][data]
//          entry = [u32 be nameLen][name][u32 be offset][u32 be size],
//          offsets are absolute; the first entry's offset equals the header
//          end (0x4f2 for 2.25.3).

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const { devtoolsDir, revisionValue } = require('../test/lib/env');

const repoRoot = path.resolve(__dirname, '..');

function parseArgs(argv) {
  const options = {
    resources: process.env.SKYLINE_DEVTOOLS_RESOURCES || path.join(devtoolsDir(), 'resources'),
    out: null,
    vendor: process.env.SKYLINE_WXLIB_VERSION || revisionValue('WXLIB_VERSION'),
    list: false,
    quiet: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--resources') options.resources = argv[++i];
    else if (arg === '--out') options.out = argv[++i];
    else if (arg === '--vendor') options.vendor = argv[++i];
    else if (arg === '--list') options.list = true;
    else if (arg === '--quiet') options.quiet = true;
    else if (arg === '-h' || arg === '--help') {
      console.log(fs.readFileSync(__filename, 'utf8').split('\n')
        .slice(28, 40).join('\n'));
      process.exit(0);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  return options;
}

// -- asar --------------------------------------------------------------------

function readAsarHeader(fd) {
  const prefix = Buffer.alloc(64);
  fs.readSync(fd, prefix, 0, prefix.length, 0);
  const jsonStart = prefix.indexOf(0x7b);  // '{'
  if (jsonStart < 0) throw new Error('asar: no JSON header');
  // The JSON is one object; walk braces while skipping strings/escapes.
  let depth = 0;
  let inString = false;
  let escaped = false;
  let end = -1;
  let position = jsonStart;
  outer: while (true) {
    const chunk = Buffer.alloc(1 << 20);
    const read = fs.readSync(fd, chunk, 0, chunk.length, position);
    if (read <= 0) break;
    for (let i = 0; i < read; i++) {
      const byte = chunk[i];
      if (inString) {
        if (escaped) escaped = false;
        else if (byte === 0x5c) escaped = true;
        else if (byte === 0x22) inString = false;
        continue;
      }
      if (byte === 0x22) inString = true;
      else if (byte === 0x7b) depth++;
      else if (byte === 0x7d) {
        depth--;
        if (depth === 0) { end = position + i + 1; break outer; }
      }
    }
    position += read;
  }
  if (end < 0) throw new Error('asar: unterminated JSON header');
  const json = Buffer.alloc(end - jsonStart);
  fs.readSync(fd, json, 0, json.length, jsonStart);
  const header = JSON.parse(json.toString('utf8'));
  // File data offsets are relative to the 4-aligned header end.
  return { header, dataStart: Math.ceil(end / 4) * 4 };
}

function asarEntries(header) {
  const entries = new Map();
  (function walk(node, prefix) {
    if (!node.files) {
      entries.set(prefix, {
        offset: Number(node.offset || 0),
        size: Number(node.size || 0),
        unpacked: !!node.unpacked,
      });
      return;
    }
    for (const [name, child] of Object.entries(node.files)) {
      walk(child, `${prefix}/${name}`);
    }
  })(header, '');
  return entries;
}

function readAsarFile(fd, dataStart, entries, entryPath) {
  const entry = entries.get(entryPath);
  if (!entry || entry.unpacked || entry.size <= 0) return null;
  const buffer = Buffer.alloc(entry.size);
  fs.readSync(fd, buffer, 0, entry.size, dataStart + entry.offset);
  return buffer;
}

// -- wxvpkg ------------------------------------------------------------------

function parseWxvpkg(buffer) {
  const count = buffer.readUInt16BE(0x10);
  const files = new Map();
  let cursor = 0x12;
  let minOffset = Infinity;
  for (let i = 0; i < count; i++) {
    const nameLen = buffer.readUInt32BE(cursor);
    cursor += 4;
    const name = buffer.toString('utf8', cursor, cursor + nameLen);
    cursor += nameLen;
    const offset = buffer.readUInt32BE(cursor);
    const size = buffer.readUInt32BE(cursor + 4);
    cursor += 8;
    files.set(name, { offset, size });
    minOffset = Math.min(minOffset, offset);
  }
  return { count, files, headerEnd: cursor, firstDataOffset: minOffset };
}

function readWxvpkgFile(buffer, files, name) {
  const entry = files.get(name);
  if (!entry) return null;
  return buffer.subarray(entry.offset, entry.offset + entry.size);
}

// Decodes the JS string literal in
//   __webviewEngine__.setGlobalStyle("WASkylineStyle", "...css...")
function decodeGlobalStyle(source) {
  const call = '__webviewEngine__.setGlobalStyle(';
  const callAt = source.indexOf(call);
  if (callAt < 0) throw new Error('setGlobalStyle(...) wrapper not found');
  let cursor = callAt + call.length;
  // Skip the stylesheet name string.
  cursor = skipJsString(source, cursor);
  while (cursor < source.length && source[cursor] !== '"' &&
         source[cursor] !== "'") cursor++;
  const quote = source[cursor];
  const start = cursor + 1;
  let index = start;
  while (index < source.length) {
    const c = source[index];
    if (c === '\\') { index += 2; continue; }
    if (c === quote) break;
    index++;
  }
  if (index >= source.length) throw new Error('unterminated CSS string');
  const literal = source.slice(start, index);
  return unescapeJsString(literal, quote);
}

function skipJsString(source, cursor) {
  while (cursor < source.length && source[cursor] !== '"' &&
         source[cursor] !== "'") cursor++;
  const quote = source[cursor];
  cursor++;
  while (cursor < source.length) {
    const c = source[cursor];
    if (c === '\\') { cursor += 2; continue; }
    cursor++;
    if (c === quote) break;
  }
  return cursor;
}

function unescapeJsString(text, quote) {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c !== '\\') { out += c; continue; }
    const next = text[++i];
    switch (next) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '0': out += '\0'; break;
      case 'x': {
        out += String.fromCharCode(parseInt(text.substr(i + 1, 2), 16));
        i += 2;
        break;
      }
      case 'u': {
        if (text[i + 1] === '{') {
          const close = text.indexOf('}', i + 2);
          out += String.fromCodePoint(parseInt(text.slice(i + 2, close), 16));
          i = close;
        } else {
          out += String.fromCharCode(parseInt(text.substr(i + 1, 4), 16));
          i += 4;
        }
        break;
      }
      case '\n': break;  // line continuation
      default: out += next === quote ? quote : next;
    }
  }
  return out;
}

// -- version selection -------------------------------------------------------

function compareVersions(a, b) {
  const left = a.split('.').map(Number);
  const right = b.split('.').map(Number);
  for (let i = 0; i < Math.max(left.length, right.length); i++) {
    const delta = (left[i] || 0) - (right[i] || 0);
    if (delta !== 0) return delta;
  }
  return 0;
}

function vendorPackages(entries) {
  const packages = [];
  for (const entryPath of entries.keys()) {
    const match = /^\/js\/vendor\/(.+)\.wxvpkg$/.exec(entryPath);
    if (match) packages.push({ version: match[1], path: entryPath });
  }
  return packages.sort((a, b) => compareVersions(a.version, b.version));
}

function devtoolsVersion(fd, dataStart, entries, resources) {
  const entryPath = '/package.json';
  const entry = entries.get(entryPath);
  let raw = null;
  if (entry && entry.unpacked) {
    const unpacked = path.join(resources, 'app.asar.unpacked',
      entryPath.replace(/^\//, ''));
    if (fs.existsSync(unpacked)) raw = fs.readFileSync(unpacked);
  }
  if (!raw) raw = readAsarFile(fd, dataStart, entries, entryPath);
  if (!raw) return null;
  try {
    const pkg = JSON.parse(raw.toString('utf8'));
    return pkg.version || null;
  } catch {
    return null;
  }
}

function readVendorVersion(archive) {
  const files = parseWxvpkg(archive).files;
  const raw = readWxvpkgFile(archive, files, '/WAVersion.json');
  if (!raw) return null;
  try {
    return JSON.parse(raw.toString('utf8'));
  } catch {
    return null;
  }
}

function sha256(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

function main() {
  let options;
  try {
    options = parseArgs(process.argv.slice(2));
  } catch (error) {
    console.error(String(error.message || error));
    process.exit(2);
  }
  const asarPath = path.join(options.resources, 'app.asar');
  if (!fs.existsSync(asarPath)) {
    console.error(`app.asar not found: ${asarPath}`);
    process.exit(1);
  }
  const fd = fs.openSync(asarPath, 'r');
  try {
    const { header, dataStart } = readAsarHeader(fd);
    const entries = asarEntries(header);
    const packages = vendorPackages(entries);
    if (options.list) {
      for (const pkg of packages) {
        const archive = readAsarFile(fd, dataStart, entries, pkg.path);
        const info = archive ? readVendorVersion(archive) : null;
        console.log([
          pkg.version,
          info && info.updateTime ? info.updateTime : 'unknown-update',
          info && info.version ? `WAVersion=${info.version}` : 'WAVersion=?',
          pkg.path,
        ].join('\t'));
      }
      return;
    }
    if (packages.length === 0) {
      console.error('no js/vendor/*.wxvpkg found in app.asar');
      process.exit(1);
    }
    const selected = options.vendor
      ? packages.find((pkg) => pkg.version === options.vendor)
      : packages[packages.length - 1];
    if (!selected) {
      console.error(`vendor ${options.vendor} not found; available: ` +
        packages.map((pkg) => pkg.version).join(', '));
      process.exit(1);
    }

    const archive = readAsarFile(fd, dataStart, entries, selected.path);
    if (!archive) throw new Error(`cannot read ${selected.path}`);
    const wxvpkg = parseWxvpkg(archive);
    const versionRaw = readWxvpkgFile(archive, wxvpkg.files, '/WAVersion.json');
    if (!versionRaw) throw new Error('WAVersion.json missing from wxvpkg');
    const versionInfo = JSON.parse(versionRaw.toString('utf8'));
    const wxlibVersion = versionInfo.version || selected.version;
    const appVersion = devtoolsVersion(fd, dataStart, entries,
      options.resources);

    const outDir = options.out || path.join(
      repoRoot, 'tmp', 'wxlib-style', wxlibVersion);
    fs.mkdirSync(outDir, { recursive: true });

    const wanted = [
      '/WASkylineStyle.css.js',
      '/WASkylineStyle.fpcssb',
      '/WASkylineStyleV2.css.js',
      '/WASkylineStyleV2.fpcssb',
      '/WAStyleIndex.fpiib',
    ];
    const files = [];
    for (const name of wanted) {
      const raw = readWxvpkgFile(archive, wxvpkg.files, name);
      if (!raw) continue;
      const outName = name.replace(/^\//, '');
      fs.writeFileSync(path.join(outDir, outName), raw);
      files.push({ name, size: raw.length, sha256: sha256(raw) });
    }

    // Readable CSS for the two stylesheet entries.
    for (const [sourceName, cssName] of [
      ['/WASkylineStyle.css.js', 'WASkylineStyle.css'],
      ['/WASkylineStyleV2.css.js', 'WASkylineStyleV2.css'],
    ]) {
      const raw = readWxvpkgFile(archive, wxvpkg.files, sourceName);
      if (!raw) continue;
      const css = decodeGlobalStyle(raw.toString('utf8'));
      fs.writeFileSync(path.join(outDir, cssName), css + '\n');
      files.push({ name: cssName, size: Buffer.byteLength(css) + 1,
                   sha256: sha256(css + '\n'),
                   decodedFrom: sourceName, version: wxlibVersion });
    }

    const manifest = {
      comment: 'wxlib base styles extracted from the WeChat DevTools vendor ' +
        'archive. The vendor package is versioned independently from the ' +
        'DevTools app; this directory and every entry carry that version.',
      version: wxlibVersion,
      updateTime: versionInfo.updateTime || null,
      devtoolsVersion: appVersion,
      source: {
        asar: path.relative(repoRoot, asarPath),
        wxvpkg: selected.path,
      },
      wxvpkg: {
        entryCount: wxvpkg.count,
        headerEnd: wxvpkg.headerEnd,
        firstDataOffset: wxvpkg.firstDataOffset,
      },
      extractedAt: new Date().toISOString(),
      files,
    };
    fs.writeFileSync(path.join(outDir, 'manifest.json'),
      JSON.stringify(manifest, null, 1) + '\n');

    if (!options.quiet) {
      console.log(`WASkylineStyle ${wxlibVersion} ` +
        `(updateTime ${versionInfo.updateTime || 'unknown'}, ` +
        `DevTools ${appVersion || 'unknown'})`);
      console.log(`source: ${selected.path} (${archive.length} bytes, ` +
        `${wxvpkg.count} entries)`);
      console.log(`out: ${outDir}`);
      for (const file of files) {
        console.log(`  ${file.name}\t${file.size}`);
      }
    }
  } finally {
    fs.closeSync(fd);
  }
}

try {
  main();
} catch (error) {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
}

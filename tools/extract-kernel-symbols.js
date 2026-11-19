#!/usr/bin/env node
'use strict';

// Extracts the phase-2 symbol inventory from the official Windows bundle:
//
//   * FFI symbols looked up from skyline.node (package:skyline sources embedded in
//     the 72 MB kernel_blob.bin);
//   * Dart callbacks registered into the native FFI table (with stringHash values);
//   * FFI typedefs / Struct layouts used by the package;
//   * the engine's @Native(...) symbol table from isolate_snapshot_data;
//   * the custom dart:ui members added by the private Skyline engine.
//
// The kernel binary is a concatenation of components (footer size chain); each
// component has a string table and an optional uriToSource section.  Everything is
// parsed locally, without third-party packages or network access.
//
// Usage:
//   node tools/extract-kernel-symbols.js            # summary
//   node tools/extract-kernel-symbols.js --write    # write spec/kernel-symbols.json
//                                                   # and docs/engine-symbols.md
//   node tools/extract-kernel-symbols.js --check    # verify committed output
//
// Discovery can be overridden with SKYLINE_KERNEL_BLOB, SKYLINE_ISOLATE_SNAPSHOT and
// SKYLINE_WIN_ADDON.  Without the official bundle, --check only validates the
// committed JSON's internal invariants.

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const repoRoot = path.resolve(__dirname, '..');
const revisionPath = path.join(repoRoot, 'native', 'engine', 'REVISION');
const outputJsonPath = path.join(repoRoot, 'spec', 'kernel-symbols.json');
const outputMarkdownPath = path.join(repoRoot, 'docs', 'engine-symbols.md');

// ---------------------------------------------------------------------------
// Kernel binary format
// ---------------------------------------------------------------------------

const KERNEL_MAGIC = 0x90abcdef;
const KERNEL_FORMAT_VERSION = 0x7d;

function readU32(buf, offset) {
  return buf.readUInt32BE(offset);
}

function readU30(buf, offset) {
  const first = buf[offset];
  if ((first & 0x80) === 0) return [first, offset + 1];
  if ((first & 0x40) === 0) {
    return [((first & 0x3f) << 8) | buf[offset + 1], offset + 2];
  }
  return [
    ((first & 0x3f) << 24) |
      (buf[offset + 1] << 16) |
      (buf[offset + 2] << 8) |
      buf[offset + 3],
    offset + 4,
  ];
}

function readByteList(buf, offset) {
  const [length, dataOffset] = readU30(buf, offset);
  if (dataOffset + length > buf.length) {
    throw new Error(`byte list past end (${dataOffset}+${length})`);
  }
  return [buf.subarray(dataOffset, dataOffset + length), dataOffset + length];
}

function decodeUtf8(buf) {
  return buf.toString('utf8');
}

function indexComponents(buf, warnings) {
  const components = [];
  let end = buf.length;
  while (end > 0) {
    if (end < 4) throw new Error(`kernel footer truncated at ${end}`);
    const size = readU32(buf, end - 4);
    const start = end - size;
    if (size <= 0 || start < 0) {
      throw new Error(`invalid component size ${size} at ${end - 4}`);
    }
    components.push({ start, size });
    end = start;
  }
  components.reverse();
  if (components.length === 0) warnings.push('kernel has no components');
  return components;
}

function parseComponentIndex(buf, component) {
  const { start, size } = component;
  const end = start + size;
  if (readU32(buf, end - 4) !== size) {
    throw new Error(`component size footer mismatch at ${start}`);
  }
  const libraryCount = readU32(buf, end - 8);
  if (libraryCount > 1_000_000) {
    throw new Error(`implausible library count ${libraryCount} at ${start}`);
  }
  // 9 fixed u32 fields + (libraryCount + 1) library offsets + 2 trailing u32.
  const indexStart = end - 8 - 4 * (libraryCount + 10);
  if (indexStart < start) throw new Error(`component index out of range at ${start}`);
  const fields = [];
  for (let i = 0; i < 9; i++) fields.push(readU32(buf, indexStart + i * 4));
  return {
    libraryCount,
    sourceTableOffset: start + fields[0],
    stringTableOffset: start + fields[6],
  };
}

function parseStringTable(buf, offset) {
  const [count, afterCount] = readU30(buf, offset);
  if (count > 10_000_000) throw new Error(`implausible string count ${count}`);
  const endOffsets = new Array(count);
  let cursor = afterCount;
  for (let i = 0; i < count; i++) {
    const [value, next] = readU30(buf, cursor);
    endOffsets[i] = value;
    cursor = next;
  }
  const dataStart = cursor;
  const strings = new Array(count);
  let previous = 0;
  for (let i = 0; i < count; i++) {
    const end = endOffsets[i];
    if (end < previous || dataStart + end > buf.length) {
      throw new Error(`string table entry ${i} out of range`);
    }
    strings[i] = decodeUtf8(buf.subarray(dataStart + previous, dataStart + end));
    previous = end;
  }
  return strings;
}

function parseSources(buf, offset) {
  const count = readU32(buf, offset);
  if (count > 100_000) throw new Error(`implausible source count ${count}`);
  let cursor = offset + 4;
  const sources = [];
  for (let i = 0; i < count; i++) {
    const [uriLength, uriStart] = readU30(buf, cursor);
    if (uriLength > 4096) throw new Error(`implausible source uri length ${uriLength}`);
    const uri = decodeUtf8(buf.subarray(uriStart, uriStart + uriLength));
    cursor = uriStart + uriLength;

    const [sourceBytes, sourceStart] = readByteList(buf, cursor);
    cursor = sourceStart;
    if (sourceStart > buf.length) throw new Error('source past end');

    const [lineCount, afterLines] = readU30(buf, cursor);
    cursor = afterLines;
    for (let j = 0; j < lineCount; j++) {
      const [, next] = readU30(buf, cursor);
      cursor = next;
    }

    const [importLength, importStart] = readU30(buf, cursor);
    const importUri = decodeUtf8(buf.subarray(importStart, importStart + importLength));
    cursor = importStart + importLength;

    const [coverageCount, afterCoverage] = readU30(buf, cursor);
    cursor = afterCoverage;
    for (let j = 0; j < coverageCount; j++) {
      const [, next] = readU30(buf, cursor);
      cursor = next;
    }

    if (sourceBytes.length > 0) {
      sources.push({
        uri,
        importUri,
        source: decodeUtf8(sourceBytes),
      });
    }
  }
  return sources;
}

function parseKernel(buf, warnings) {
  const components = indexComponents(buf, warnings);
  const sdkHashes = new Set();
  let strings = 0;
  let sourceFiles = 0;
  const sources = [];
  for (const component of components) {
    const { start, size } = component;
    const end = start + size;
    try {
      if (readU32(buf, start) !== KERNEL_MAGIC) {
        warnings.push(`component at ${start} has unexpected magic`);
        continue;
      }
      const formatVersion = readU32(buf, start + 4);
      if (formatVersion !== KERNEL_FORMAT_VERSION) {
        warnings.push(`component at ${start} uses kernel format ${formatVersion}`);
      }
      sdkHashes.add(buf.toString('ascii', start + 8, start + 18));
      const index = parseComponentIndex(buf, component);
      const table = parseStringTable(buf, index.stringTableOffset);
      strings += table.length;
      const componentSources = parseSources(buf, index.sourceTableOffset);
      sourceFiles += componentSources.length;
      sources.push(...componentSources);
    } catch (error) {
      warnings.push(`skipping component at ${start}: ${error.message}`);
    }
  }
  return { components: components.length, sdkHashes: [...sdkHashes].sort(), strings, sourceFiles, sources };
}

// ---------------------------------------------------------------------------
// PE export table
// ---------------------------------------------------------------------------

function parsePeExports(buf) {
  if (buf.length < 0x40 || buf.readUInt16LE(0) !== 0x5a4d) return null; // MZ
  const peOffset = buf.readUInt32LE(0x3c);
  if (peOffset + 24 > buf.length) return null;
  if (buf.toString('ascii', peOffset, peOffset + 4) !== 'PE\0\0') return null;

  const coff = peOffset + 4;
  const sectionCount = buf.readUInt16LE(coff + 2);
  const optionalSize = buf.readUInt16LE(coff + 16);
  const optional = coff + 20;
  const magic = buf.readUInt16LE(optional);
  if (magic !== 0x20b) return null; // PE32+ only; skyline.node is x64
  const dataDirectory = optional + 112;
  const exportRva = buf.readUInt32LE(dataDirectory);
  if (exportRva === 0) return null;

  const sectionTable = optional + optionalSize;
  const sections = [];
  for (let i = 0; i < sectionCount; i++) {
    const offset = sectionTable + i * 40;
    if (offset + 40 > buf.length) return null;
    sections.push({
      virtualSize: buf.readUInt32LE(offset + 8),
      virtualAddress: buf.readUInt32LE(offset + 12),
      rawSize: buf.readUInt32LE(offset + 16),
      rawOffset: buf.readUInt32LE(offset + 20),
    });
  }
  const rvaToOffset = rva => {
    for (const section of sections) {
      const span = Math.max(section.virtualSize, section.rawSize);
      if (rva >= section.virtualAddress && rva < section.virtualAddress + span) {
        return section.rawOffset + (rva - section.virtualAddress);
      }
    }
    throw new Error(`RVA ${rva.toString(16)} is not mapped`);
  };

  const exportOffset = rvaToOffset(exportRva);
  const numberOfFunctions = buf.readUInt32LE(exportOffset + 20);
  const numberOfNames = buf.readUInt32LE(exportOffset + 24);
  const addressOfNames = rvaToOffset(buf.readUInt32LE(exportOffset + 32));
  const names = [];
  for (let i = 0; i < numberOfNames; i++) {
    const nameRva = buf.readUInt32LE(addressOfNames + i * 4);
    const nameOffset = rvaToOffset(nameRva);
    let end = nameOffset;
    while (end < buf.length && buf[end] !== 0) end++;
    names.push(buf.toString('ascii', nameOffset, end));
  }
  names.sort();
  return { numberOfFunctions, numberOfNames, namedExports: names };
}

// ---------------------------------------------------------------------------
// Official isolate snapshot: @Native symbols + patched dart:ui additions
// ---------------------------------------------------------------------------

function printableRuns(buf, minLength = 6) {
  const runs = [];
  let start = -1;
  for (let i = 0; i <= buf.length; i++) {
    const byte = i < buf.length ? buf[i] : 0;
    const printable = byte >= 0x20 && byte <= 0x7e;
    if (printable && start < 0) start = i;
    if (!printable && start >= 0) {
      if (i - start >= minLength) runs.push(buf.toString('latin1', start, i));
      start = -1;
    }
  }
  return runs;
}

function extractBalancedBlock(text, startIndex) {
  const open = text.indexOf('{', startIndex);
  if (open < 0) return null;
  let depth = 1;
  let quote = null;
  let lineComment = false;
  for (let i = open + 1; i < text.length; i++) {
    const ch = text[i];
    if (lineComment) {
      if (ch === '\n') lineComment = false;
      continue;
    }
    if (quote !== null) {
      if (ch === '\\') {
        i++;
      } else if (ch === quote) {
        quote = null;
      }
      continue;
    }
    if (ch === '/' && text[i + 1] === '/') {
      lineComment = true;
      i++;
      continue;
    }
    if (ch === "'" || ch === '"') {
      quote = ch;
      continue;
    }
    if (ch === '{') depth++;
    if (ch === '}') {
      depth--;
      if (depth === 0) return text.slice(open + 1, i);
    }
  }
  return null;
}

function extractClassMembers(text) {
  const appIndex = text.indexOf('class Application {');
  if (appIndex < 0) return [];
  const body = extractBalancedBlock(text, appIndex);
  if (body === null) return [];

  const members = new Set();
  const lines = body.split('\n');
  let depth = 0;
  for (const rawLine of lines) {
    const line = rawLine.replace(/\/\/.*$/, '');
    const trimmed = line.trim();
    if (depth === 0 && trimmed.length > 0) {
      const getter = /^\s*(?:static\s+)?[\w<>?.,\[\]]+\s+get\s+(\w+)/.exec(line);
      const setter = /^\s*set\s+(\w+)/.exec(line);
      const method = /^\s*(?:static\s+)?(?:[\w<>?.,\[\]]+\s+)+([A-Za-z_]\w*)(?:<[^>]*>)?\s*\(/.exec(line);
      const field = /^\s*(?:static\s+)?(?:final\s+|late\s+)?(?:[\w<>?.,\[\]]+\s+)+(_?[A-Za-z]\w*)\s*(?:;|=)/.exec(line);
      if (getter) members.add(getter[1]);
      if (setter) members.add(setter[1]);
      if (method && !method[1].includes('.')) members.add(method[1]);
      if (field) members.add(field[1]);
    }
    for (const ch of line) {
      if (ch === '{') depth++;
      if (ch === '}') depth = Math.max(0, depth - 1);
    }
  }
  return [...members].sort();
}

function parseSnapshot(buf) {
  const symbolText = printableRuns(buf, 6).join('\n');
  const symbols = new Set();
  const symbolPattern = /symbol:\s*['"]([^'"]+)['"]/g;
  for (let match = symbolPattern.exec(symbolText); match; match = symbolPattern.exec(symbolText)) {
    const name = match[1];
    if (/^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z0-9_]+)+$/.test(name)) {
      symbols.add(name);
    } else if (/^(DartPluginRegistrant_|FinalizerEntry_|Pointer_asTypedListFinalizer)/.test(name)) {
      symbols.add(name);
    }
  }

  // Source text in a Dart snapshot is fragmented; short runs such as "  }" are
  // below the symbol-scanning threshold but are needed to balance the class body.
  const appByte = buf.indexOf(Buffer.from('class Application {'));
  let applicationMembers = [];
  if (appByte >= 0) {
    const marker = buf.indexOf(Buffer.from('dart:ui/application.dart'), appByte + 1);
    const endByte = marker > appByte ? marker : Math.min(buf.length, appByte + 20000);
    const appText = printableRuns(buf.subarray(appByte, endByte), 1).join('\n');
    applicationMembers = extractClassMembers(appText);
  }

  const custom = [];
  if (appByte >= 0) {
    custom.push({
      name: 'Application',
      kind: 'class',
      source: 'dart:ui/application.dart',
      members: applicationMembers,
    });
  }
  if (/Paragraph\s+createNativeParagraph\s*\(\s*\)/.test(symbolText)) {
    custom.push({
      name: 'createNativeParagraph',
      kind: 'function',
      source: 'dart:ui/painting.dart',
      signature: 'Paragraph createNativeParagraph()',
    });
  }
  if (/external\s+void\s+warmUpFont\s*\(/.test(symbolText)) {
    custom.push({
      name: 'warmUpFont',
      kind: 'function',
      source: 'dart:ui/painting.dart',
      signature: 'void warmUpFont(String cacheDir)',
    });
  }

  return { nativeSymbols: [...symbols].sort(), customMembers: custom };
}

// ---------------------------------------------------------------------------
// Dart source scanning
// ---------------------------------------------------------------------------

const SMI_MASK = 0x3fffffffn;

function mixHashCodeBits(existing, value) {
  let h = BigInt(value) & 0x1fffffffn;
  h += ((h & 0x3fffn) << 15n) ^ 0x1fffcd7dn;
  h ^= h >> 10n;
  h += (h & 0x3ffffffn) << 3n;
  h ^= h >> 6n;
  h += ((h & 0x7ffffffn) << 2n) + ((h & 0x7fffn) << 14n);
  h ^= h >> 16n;
  const high = existing >> 15n;
  const low = existing & 0x7fffn;
  return ((high * 13n) ^ (low * 997n) ^ h) & SMI_MASK;
}

// Mirrors package:skyline/utils/dart_string_hash.dart.
function stringHash(value) {
  let hash = mixHashCodeBits(0n, value.length);
  for (let i = 0; i < value.length; i++) {
    hash = mixHashCodeBits(hash, value.charCodeAt(i));
  }
  return Number(hash);
}

function compareStrings(a, b) {
  return a < b ? -1 : a > b ? 1 : 0;
}

function lineAt(source, index) {
  let line = 1;
  for (let i = 0; i < index; i++) {
    if (source.charCodeAt(i) === 10) line++;
  }
  return line;
}

function addSource(map, name, file, line) {
  if (!map.has(name)) map.set(name, []);
  const entry = map.get(name);
  if (!entry.some(item => item.file === file && item.line === line)) {
    entry.push({ file, line });
  }
}

function scanLookupSymbols(sources) {
  const found = new Map();
  const pattern = /lookupFunction\s*<[\s\S]*?>\s*\(\s*['"]([A-Za-z_][A-Za-z0-9_]*)['"]/g;
  for (const source of sources) {
    for (let match = pattern.exec(source.source); match; match = pattern.exec(source.source)) {
      addSource(found, match[1], source.importUri, lineAt(source.source, match.index));
    }
  }
  return found;
}

function scanCallbacks(source) {
  const found = new Map();
  const pattern = /\bR\(\s*['"]([A-Za-z_][A-Za-z0-9_]*)['"]/g;
  for (let match = pattern.exec(source); match; match = pattern.exec(source)) {
    addSource(found, match[1], 'package:skyline/core/ffi_registry.dart', lineAt(source, match.index));
  }
  return found;
}

function scanTypedefs(sources) {
  const found = new Map();
  const pattern = /^typedef\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([\s\S]*?);/gm;
  for (const source of sources) {
    for (let match = pattern.exec(source.source); match; match = pattern.exec(source.source)) {
      const name = match[1];
      if (!found.has(name)) {
        found.set(name, {
          name,
          signature: match[2].replace(/\s+/g, ' ').trim(),
          file: source.importUri,
          line: lineAt(source.source, match.index),
        });
      }
    }
  }
  return found;
}

function scanStructs(sources) {
  const found = new Map();
  const pattern = /(?:final\s+)?class\s+([A-Za-z_][A-Za-z0-9_]*)\s+extends\s+Struct\s*\{([\s\S]*?)\n\}/g;
  const fieldPattern = /external\s+([A-Za-z_][\w<>?]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;/g;
  for (const source of sources) {
    for (let match = pattern.exec(source.source); match; match = pattern.exec(source.source)) {
      const fields = [];
      for (let field = fieldPattern.exec(match[2]); field; field = fieldPattern.exec(match[2])) {
        fields.push({ type: field[1], name: field[2] });
      }
      if (!found.has(match[1])) {
        found.set(match[1], {
          name: match[1],
          fields,
          file: source.importUri,
          line: lineAt(source.source, match.index),
        });
      }
    }
  }
  return found;
}

function scanCalls(sources, needle) {
  const calls = [];
  for (const source of sources) {
    let index = source.source.indexOf(needle);
    while (index >= 0) {
      calls.push({ file: source.importUri, line: lineAt(source.source, index) });
      index = source.source.indexOf(needle, index + needle.length);
    }
  }
  return calls;
}

function scanPlatformChannels(sources) {
  const channels = new Set();
  const pattern = /MethodChannel\(\s*['"]([^'"]+)['"]/g;
  for (const source of sources) {
    for (let match = pattern.exec(source.source); match; match = pattern.exec(source.source)) {
      channels.add(match[1]);
    }
  }
  return [...channels].sort();
}

// ---------------------------------------------------------------------------
// Discovery / pins / output
// ---------------------------------------------------------------------------

function readPins() {
  const pins = {};
  if (!fs.existsSync(revisionPath)) return pins;
  for (const line of fs.readFileSync(revisionPath, 'utf8').split('\n')) {
    const match = /^([A-Z0-9_]+)=(.*)$/.exec(line);
    if (match) pins[match[1]] = match[2];
  }
  return pins;
}

function discoverInputs() {
  let oracle = null;
  try {
    oracle = require('../test/lib/env').oraclePaths();
  } catch {
    oracle = null;
  }
  const kernel = process.env.SKYLINE_KERNEL_BLOB
    || (oracle && path.join(oracle.bundle, 'flutter_assets', 'kernel_blob.bin'))
    || null;
  const snapshot = process.env.SKYLINE_ISOLATE_SNAPSHOT
    || (kernel ? path.join(path.dirname(kernel), 'isolate_snapshot_data') : null)
    || null;
  const addon = process.env.SKYLINE_WIN_ADDON
    || (oracle && oracle.addon)
    || null;
  return { kernel, snapshot, addon, oracle };
}

function sha256(buf) {
  return crypto.createHash('sha256').update(buf).digest('hex');
}

function buildInventory(warnings) {
  const inputs = discoverInputs();
  const pins = readPins();

  if (!inputs.kernel || !fs.existsSync(inputs.kernel)) {
    throw new Error(
      'official kernel_blob.bin not found; set SKYLINE_KERNEL_BLOB or SKYLINE_WIN_RESOURCES',
    );
  }
  const kernelBuf = fs.readFileSync(inputs.kernel);
  const kernel = parseKernel(kernelBuf, warnings);

  const sources = kernel.sources;
  const skylineSources = sources.filter(source => /^package:skyline\//.test(source.importUri));
  const ffiRegistry = skylineSources.find(source => source.importUri === 'package:skyline/core/ffi_registry.dart')
    || { source: '', importUri: 'package:skyline/core/ffi_registry.dart' };

  const lookupMap = scanLookupSymbols(skylineSources);
  const callbackMap = scanCallbacks(ffiRegistry.source);
  const typedefMap = scanTypedefs(skylineSources);
  const structMap = scanStructs(skylineSources);

  let addonInfo = null;
  let exportSet = null;
  if (inputs.addon && fs.existsSync(inputs.addon)) {
    const addonBuf = fs.readFileSync(inputs.addon);
    const exports = parsePeExports(addonBuf);
    if (exports) {
      exportSet = new Set(exports.namedExports);
      addonInfo = {
        sha256: sha256(addonBuf),
        size: addonBuf.length,
        namedExports: exports.numberOfNames,
        functions: exports.numberOfFunctions,
      };
    } else {
      warnings.push(`could not parse PE exports from ${path.basename(inputs.addon)}`);
    }
  } else {
    warnings.push('skyline.node not found; lookup symbols are not cross-checked against PE exports');
  }

  const lookupSymbols = [...lookupMap.entries()]
    .map(([name, entries]) => ({
      name,
      exported: exportSet ? exportSet.has(name) : null,
      sources: entries.sort((a, b) => compareStrings(a.file, b.file) || a.line - b.line),
    }))
    .sort((a, b) => compareStrings(a.name, b.name));

  const registeredCallbacks = [...callbackMap.entries()]
    .map(([name, entries]) => ({
      name,
      hash: stringHash(name),
      sources: entries.sort((a, b) => compareStrings(a.file, b.file) || a.line - b.line),
    }))
    .sort((a, b) => compareStrings(a.name, b.name));

  const typedefs = [...typedefMap.values()].sort((a, b) => compareStrings(a.name, b.name));
  const structs = [...structMap.values()].sort((a, b) => compareStrings(a.name, b.name));

  let snapshotInfo = null;
  let nativeSymbols = [];
  let customMembers = [];
  if (inputs.snapshot && fs.existsSync(inputs.snapshot)) {
    const snapshotBuf = fs.readFileSync(inputs.snapshot);
    const parsed = parseSnapshot(snapshotBuf);
    nativeSymbols = parsed.nativeSymbols;
    customMembers = parsed.customMembers;
    snapshotInfo = { sha256: sha256(snapshotBuf), size: snapshotBuf.length };
  } else {
    warnings.push('isolate_snapshot_data not found; engine native symbols are unavailable');
  }

  for (const member of customMembers) {
    if (member.kind === 'function') {
      member.calls = scanCalls(skylineSources, `ui.${member.name}`);
    } else if (member.name === 'Application') {
      member.calls = scanCalls(skylineSources, 'ui.Application');
    }
  }

  return {
    schemaVersion: 1,
    pins: {
      flutterVersion: pins.FLUTTER_VERSION || null,
      engineRevision: pins.FLUTTER_GIT_REV || null,
      dartVersion: pins.DART_VERSION || null,
      dartGitRev: pins.DART_GIT_REV || null,
      officialKernelSdkHash: pins.OFFICIAL_KERNEL_SDK_HASH || null,
      publicKernelSdkHash: pins.PUBLIC_KERNEL_SDK_HASH || null,
      skylineVersion: pins.SKYLINE_VERSION || null,
      skylineGitRev: pins.SKYLINE_GIT_REV || null,
      officialEngineGitRev: pins.OFFICIAL_ENGINE_GIT_REV || null,
    },
    kernel: {
      sha256: sha256(kernelBuf),
      size: kernelBuf.length,
      components: kernel.components,
      sdkHashes: kernel.sdkHashes,
      strings: kernel.strings,
      sourceFiles: kernel.sourceFiles,
      skylineSourceFiles: skylineSources.length,
    },
    addon: addonInfo,
    snapshot: snapshotInfo,
    ffi: {
      lookupSymbols,
      registeredCallbacks,
      typedefs,
      structs,
    },
    engineNativeSymbols: nativeSymbols.map(name => ({
      name,
      source: 'isolate_snapshot_data',
    })),
    dartUiCustomMembers: customMembers,
    platformChannels: scanPlatformChannels(skylineSources),
    warnings,
  };
}

function validateInventory(inventory) {
  const errors = [];
  if (inventory.schemaVersion !== 1) errors.push('schemaVersion must be 1');
  const lookups = inventory.ffi && inventory.ffi.lookupSymbols;
  if (!Array.isArray(lookups) || lookups.length === 0) {
    errors.push('ffi.lookupSymbols is empty');
  } else {
    const names = new Set();
    for (const entry of lookups) {
      if (!entry.name) errors.push('lookup without name');
      if (names.has(entry.name)) errors.push(`duplicate lookup ${entry.name}`);
      names.add(entry.name);
      if (entry.exported === false) errors.push(`lookup ${entry.name} is not exported by skyline.node`);
    }
    if (!names.has('InitDartAPIDLSkyline')) errors.push('InitDartAPIDLSkyline missing');
  }
  const callbacks = inventory.ffi && inventory.ffi.registeredCallbacks;
  if (!Array.isArray(callbacks) || callbacks.length === 0) {
    errors.push('ffi.registeredCallbacks is empty');
  }
  if (!Array.isArray(inventory.engineNativeSymbols)) {
    errors.push('engineNativeSymbols must be an array');
  }
  return errors;
}

function renderMarkdown(inventory) {
  const lines = [];
  const push = (...args) => lines.push(...args);
  push(
    '# Engine / FFI symbol inventory',
    '',
    '> Generated by `node tools/extract-kernel-symbols.js --write`.  Do not edit by',
    '> hand; regenerate after changing the pinned engine or the official bundle.',
    '',
    '## Provenance',
    '',
    '| field | value |',
    '| --- | --- |',
    `| Flutter | ${inventory.pins.flutterVersion} |`,
    `| engine revision | \`${inventory.pins.engineRevision}\` |`,
    `| Dart | ${inventory.pins.dartVersion} (\`${inventory.pins.dartGitRev}\`) |`,
    `| official kernel SDK hash | \`${inventory.pins.officialKernelSdkHash}\` |`,
    `| public patched-SDK hash | \`${inventory.pins.publicKernelSdkHash}\` |`,
    `| Skyline | ${inventory.pins.skylineVersion} (\`${inventory.pins.skylineGitRev}\`) |`,
    `| private engine rev | \`${inventory.pins.officialEngineGitRev}\` (not public) |`,
    '',
    '## Inputs',
    '',
    `- kernel: ${inventory.kernel.size} bytes, \`${inventory.kernel.sha256}\`, ` +
      `${inventory.kernel.components} components, ${inventory.kernel.strings} strings, ` +
      `${inventory.kernel.skylineSourceFiles} package:skyline sources.`,
  );
  if (inventory.addon) {
    push(
      `- addon: ${inventory.addon.size} bytes, \`${inventory.addon.sha256}\`, ` +
        `${inventory.addon.namedExports} named exports / ${inventory.addon.functions} functions.`,
    );
  }
  if (inventory.engineNativeSymbols.length > 0) {
    push(
      `- engine native symbols: ${inventory.engineNativeSymbols.length}` +
        (inventory.snapshot
          ? ` (from ${inventory.snapshot.size} bytes, \`${inventory.snapshot.sha256}\`).`
          : '.'),
    );
  }
  push('');

  push('## FFI lookup symbols', '');
  push('These names are passed to `DynamicLibrary.lookupFunction` by `package:skyline`.');
  push('Every entry is present in the official `skyline.node` PE export table.', '');
  push('| symbol | exported | first source |', '| --- | --- | --- |');
  for (const entry of inventory.ffi.lookupSymbols) {
    const first = entry.sources[0];
    const exported = entry.exported === null ? '?' : entry.exported ? 'yes' : '**no**';
    push(`| \`${entry.name}\` | ${exported} | \`${first ? first.file : ''}\`:${first ? first.line : ''} |`);
  }
  push('');

  push('## Registered callbacks', '');
  push(
    `\`setupFFIFunctions\` registers ${inventory.ffi.registeredCallbacks.length} Dart callbacks ` +
      'into the native FFI table with `stringHash(name)`.  The full list and hashes are in',
    '`spec/kernel-symbols.json`; the prefixes are:',
    '',
  );
  const prefixes = new Map();
  for (const entry of inventory.ffi.registeredCallbacks) {
    const match = /^([A-Z][a-z]+|[A-Z]+|[a-z]+)/.exec(entry.name);
    const prefix = match ? match[1] : entry.name;
    prefixes.set(prefix, (prefixes.get(prefix) || 0) + 1);
  }
  push(
    '| prefix | count |',
    '| --- | --- |',
    ...[...prefixes.entries()]
      .sort((a, b) => b[1] - a[1] || compareStrings(a[0], b[0]))
      .slice(0, 30)
      .map(([prefix, count]) => `| ${prefix} | ${count} |`),
    '',
  );

  push('## Engine @Native symbols', '');
  push(
    'Extracted from the official `isolate_snapshot_data`.  These are the native',
    'functions the engine resolves for the patched `dart:ui`; any reimplementation',
    'that runs the official kernel must provide them (or stubs).',
    '',
  );
  const groups = new Map();
  for (const { name } of inventory.engineNativeSymbols) {
    const group = name.includes('::') ? name.split('::')[0] : '(top-level)';
    groups.set(group, (groups.get(group) || 0) + 1);
  }
  push(
    '| group | count |',
    '| --- | --- |',
    ...[...groups.entries()]
      .sort((a, b) => b[1] - a[1] || compareStrings(a[0], b[0]))
      .map(([group, count]) => `| \`${group}\` | ${count} |`),
    '',
  );

  push('## Custom dart:ui additions', '');
  push(
    'The private engine patches `dart:ui` with an `Application` abstraction and two',
    'extra top-level functions.  The official kernel references these, so a public',
    'engine must add compatible declarations (and native implementations) before it',
    'can run that kernel.',
    '',
    '| member | kind | source | notes |',
    '| --- | --- | --- | --- |',
  );
  for (const member of inventory.dartUiCustomMembers) {
    let note = '';
    if (member.kind === 'class' && member.members) {
      note = `members: ${member.members.map(name => `\`${name}\``).join(', ')}`;
    } else if (member.signature) {
      note = `\`${member.signature}\``;
    }
    if (member.calls && member.calls.length > 0) {
      note += `; calls: ${member.calls.map(call => `\`${call.file}:${call.line}\``).join(', ')}`;
    }
    push(`| \`${member.name}\` | ${member.kind} | \`${member.source}\` | ${note} |`);
  }
  if (inventory.platformChannels.length > 0) {
    push('', 'Platform channels referenced by `package:skyline`: ' +
      inventory.platformChannels.map(name => `\`${name}\``).join(', ') + '.');
  }
  push('');

  if (inventory.warnings.length > 0) {
    push('## Warnings', '', ...inventory.warnings.map(warning => `- ${warning}`), '');
  }
  push(
    '## Regenerate',
    '',
    '```bash',
    'node tools/extract-kernel-symbols.js --write',
    'node tools/extract-kernel-symbols.js --check',
    '```',
    '',
  );
  return lines.join('\n');
}

function summarize(inventory) {
  const exported = inventory.ffi.lookupSymbols.filter(entry => entry.exported).length;
  const missing = inventory.ffi.lookupSymbols.filter(entry => entry.exported === false);
  return [
    `kernel: ${inventory.kernel.components} components, ${inventory.kernel.strings} strings, ` +
      `${inventory.kernel.skylineSourceFiles} package:skyline sources`,
    `ffi: ${inventory.ffi.lookupSymbols.length} lookups (${exported} exported), ` +
      `${inventory.ffi.registeredCallbacks.length} callbacks, ` +
      `${inventory.ffi.typedefs.length} typedefs, ${inventory.ffi.structs.length} structs`,
    `engine native symbols: ${inventory.engineNativeSymbols.length}`,
    `custom dart:ui members: ${inventory.dartUiCustomMembers.map(member => member.name).join(', ')}`,
    missing.length > 0
      ? `MISSING EXPORTS: ${missing.map(entry => entry.name).join(', ')}`
      : 'all lookup symbols are exported',
  ].join('\n');
}

function main() {
  const argv = process.argv.slice(2);
  const write = argv.includes('--write');
  const check = argv.includes('--check');
  if (argv.includes('--help') || argv.includes('-h')) {
    console.log(
      'usage: node tools/extract-kernel-symbols.js [--write|--check]\n' +
        '  --write  regenerate spec/kernel-symbols.json and docs/engine-symbols.md\n' +
        '  --check  validate the committed file; recompute when the official bundle exists',
    );
    return 0;
  }

  if (check) {
    if (!fs.existsSync(outputJsonPath)) {
      console.error(`missing ${path.relative(repoRoot, outputJsonPath)}; run --write`);
      return 1;
    }
    const committed = JSON.parse(fs.readFileSync(outputJsonPath, 'utf8'));
    const errors = validateInventory(committed);
    if (errors.length > 0) {
      console.error(errors.join('\n'));
      return 1;
    }
    const inputs = discoverInputs();
    const haveBundle = inputs.kernel && fs.existsSync(inputs.kernel);
    if (!haveBundle) {
      console.log('committed inventory is valid; official bundle not present, skipped recompute');
      return 0;
    }
    const warnings = [];
    const fresh = buildInventory(warnings);
    const freshErrors = validateInventory(fresh);
    if (freshErrors.length > 0) {
      console.error(freshErrors.join('\n'));
      return 1;
    }
    const committedText = JSON.stringify(committed, null, 2);
    const freshText = JSON.stringify(fresh, null, 2);
    if (committedText !== freshText) {
      console.error('committed inventory is out of date; run --write');
      return 1;
    }
    console.log('inventory is up to date');
    return 0;
  }

  const warnings = [];
  const inventory = buildInventory(warnings);
  const errors = validateInventory(inventory);
  if (errors.length > 0) {
    console.error(errors.join('\n'));
    return 1;
  }
  console.log(summarize(inventory));
  if (warnings.length > 0) {
    console.error(`warnings:\n- ${warnings.join('\n- ')}`);
  }

  if (write) {
    fs.mkdirSync(path.dirname(outputJsonPath), { recursive: true });
    fs.mkdirSync(path.dirname(outputMarkdownPath), { recursive: true });
    fs.writeFileSync(outputJsonPath, JSON.stringify(inventory, null, 2) + '\n');
    fs.writeFileSync(outputMarkdownPath, renderMarkdown(inventory) + '\n');
    console.log(`wrote ${path.relative(repoRoot, outputJsonPath)} and ${path.relative(repoRoot, outputMarkdownPath)}`);
  }
  return 0;
}

if (require.main === module) {
  try {
    process.exitCode = main();
  } catch (error) {
    console.error(error.stack || error.message);
    process.exitCode = 1;
  }
}

module.exports = {
  parseKernel,
  parsePeExports,
  parseSnapshot,
  stringHash,
  buildInventory,
  validateInventory,
  discoverInputs,
};

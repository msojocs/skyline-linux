'use strict';

const { isDeepStrictEqual } = require('node:util');
const { normalizeEntry } = require('./normalize');

const COMMON_SHARED_EXPORTS = ['getMemory', 'removeMemory', 'setMemory'];
// Official Windows 2.02.2609250 surface (recorded under Wine).
const WINDOWS_SHARED_EXPORTS = [
  ...COMMON_SHARED_EXPORTS,
  'copyMemoryToBuffer',
  'deleteCopyCache',
  'deleteGetCache',
  'deleteSetCache',
  'getMemoryByAddress',
  'setMemoryByAddress',
].sort();
// The Linux port may additionally expose `version` and may omit the
// Windows-only address helpers.
const LINUX_SHARED_EXPORTS_ALLOWED = [...WINDOWS_SHARED_EXPORTS, 'version'].sort();

function comparable(entry) {
  if (!entry) return { status: 'missing' };
  const result = { status: entry.status };
  if (entry.reason) result.reason = entry.reason;
  if (entry.value !== undefined) result.value = entry.value;
  if (entry.error) result.error = { name: entry.error.name, message: entry.error.message };
  return result;
}

function diffPaths(expected, actual, prefix = '') {
  if (isDeepStrictEqual(expected, actual)) return [];
  const path = prefix || '<root>';
  if (expected && actual && typeof expected === 'object' && typeof actual === 'object'
      && !Array.isArray(expected) && !Array.isArray(actual)) {
    const keys = new Set([...Object.keys(expected), ...Object.keys(actual)]);
    const diffs = [];
    for (const key of keys) {
      diffs.push(...diffPaths(expected[key], actual[key], prefix ? `${prefix}.${key}` : key));
    }
    return diffs.length ? diffs : [`${path}: ${JSON.stringify(expected)} !== ${JSON.stringify(actual)}`];
  }
  return [`${path}: expected ${JSON.stringify(expected)}, actual ${JSON.stringify(actual)}`];
}

function ok() { return { ok: true, diffs: [] }; }
function fail(diffs) { return { ok: false, diffs }; }

function maskMessages(value) {
  if (Array.isArray(value)) return value.map(maskMessages);
  if (value && typeof value === 'object') {
    const result = {};
    for (const key of Object.keys(value)) {
      if (key === 'message' || key === 'stack') continue;
      result[key] = maskMessages(value[key]);
    }
    return result;
  }
  return value;
}

function compareIgnoringErrorMessage(expected, actual) {
  const left = maskMessages(expected);
  const right = maskMessages(actual);
  return isDeepStrictEqual(left, right) ? ok() : fail(diffPaths(left, right));
}

function dataLength(value) {
  if (!value) return undefined;
  if (value.get && value.get.byteLength !== undefined) return value.get.byteLength;
  if (value.set && value.set.byteLength !== undefined) return value.set.byteLength;
  return undefined;
}

function compareSharedShape(expected, actual) {
  const left = expected && expected.value || {};
  const right = actual && actual.value || {};
  const diffs = [];
  const leftLength = dataLength(left);
  const rightLength = dataLength(right);
  if (leftLength !== rightLength) diffs.push(`data length: ${leftLength} != ${rightLength}`);
  const rightSet = right.set || {};
  if (rightLength !== undefined && rightSet.size !== undefined && rightSet.size !== rightLength) {
    diffs.push(`descriptor size ${rightSet.size} != data length ${rightLength}`);
  }
  if (rightSet.kind === 'Uint8Array' && rightSet.pointerNonZero !== true) {
    diffs.push('memory handle must contain a non-zero process-local pointer');
  }
  const leftGet = left.get && left.get.byteLength;
  const rightGet = right.get && right.get.byteLength;
  if (leftGet !== rightGet) diffs.push(`getMemory byteLength: ${leftGet} != ${rightGet}`);
  return diffs.length ? fail(diffs) : ok();
}

function compareReuse(expected, actual) {
  function semantic(entry) {
    const value = entry && entry.value || {};
    return {
      status: entry && entry.status,
      first: value.first && (value.first.size ?? value.first.byteLength),
      second: value.second && (value.second.size ?? value.second.byteLength),
      getByteLength: value.get && value.get.byteLength,
    };
  }
  const left = semantic(expected);
  const right = semantic(actual);
  return isDeepStrictEqual(left, right) ? ok() : fail(diffPaths(left, right));
}

function compareExportSets(expected, actual) {
  const keys = actual && actual.value && actual.value.keys;
  if (!Array.isArray(keys)) return fail(['actual exports are not an array']);
  const missing = COMMON_SHARED_EXPORTS.filter(name => !keys.includes(name));
  if (missing.length) return fail([`missing required exports: ${missing.join(', ')}`]);
  const unexpected = keys.filter(name => !LINUX_SHARED_EXPORTS_ALLOWED.includes(name));
  if (unexpected.length) return fail([`unexpected exports: ${unexpected.join(', ')}`]);
  return ok();
}

function compareCopyApi(expected, actual) {
  const value = actual && actual.value || {};
  const required = ['copyMemoryToBuffer', 'deleteCopyCache'];
  const missing = required.filter(name => value[name] !== 'function');
  if (missing.length) return fail([`missing copy API: ${missing.join(', ')}`]);
  const known = [...WINDOWS_SHARED_EXPORTS, 'version'];
  const unexpected = Object.keys(value).filter(name => !known.includes(name));
  return unexpected.length ? fail([`unexpected copy API: ${unexpected.join(', ')}`]) : ok();
}

// The official module accepts any key string (including `../escape` and
// non-ASCII); the Linux port deliberately narrows the alphabet to prevent
// path traversal.  Safe keys must behave identically; unsafe keys must be
// rejected only on Linux.
function compareKeyValidation(expected, actual) {
  const win = expected && expected.value || {};
  const linux = actual && actual.value || {};
  const diffs = [];
  const safeKeys = ['"ok.key_1-2"', `"${'x'.repeat(200)}"`];
  const unsafeKeys = ['"../escape"', '"a b"', '"中文"', `"${'x'.repeat(201)}"`];
  for (const key of safeKeys) {
    if (!linux[key] || linux[key].ok !== true) diffs.push(`Linux must accept safe key ${key}`);
    if (win[key] && win[key].ok === true && linux[key] && linux[key].ok === true) {
      const size = win[key].value && win[key].value.size;
      const linuxSize = linux[key].value && (linux[key].value.size ?? linux[key].value.byteLength);
      if (size !== undefined && linuxSize !== undefined && size !== linuxSize) {
        diffs.push(`key ${key}: size ${linuxSize} != ${size}`);
      }
    }
  }
  for (const key of unsafeKeys) {
    if (linux[key] && linux[key].ok === true) diffs.push(`Linux must reject unsafe key ${key}`);
  }
  for (const key of ['""', 'number', 'null']) {
    const left = win[key] && win[key].ok;
    const right = linux[key] && linux[key].ok;
    if (left !== right) diffs.push(`${key}: Windows ok=${left}, Linux ok=${right}`);
  }
  return diffs.length ? fail(diffs) : ok();
}

function compareLength(expected, actual) {
  const left = expected && expected.value || {};
  const right = actual && actual.value || {};
  if (left.ok !== right.ok) return fail([`ok mismatch: ${left.ok} vs ${right.ok}`]);
  if (!left.ok && left.error && right.error) {
    // The official Windows module reports invalid lengths as Error while the
    // Linux module uses the more precise RangeError; both are invalid-input
    // failures that callers must handle identically.
    const families = new Set([left.error.name, right.error.name]);
    const lenient = [...families].every(name => name === 'Error' || name === 'RangeError');
    if (!lenient) return fail([`error type mismatch: ${left.error.name} vs ${right.error.name}`]);
  }
  if (left.ok) {
    const leftSize = left.value && (left.value.size ?? left.value.byteLength);
    const rightSize = right.value && (right.value.size ?? right.value.byteLength);
    if (leftSize !== rightSize) return fail([`size mismatch: ${leftSize} vs ${rightSize}`]);
  }
  return ok();
}

const POLICIES = {
  'shm.exports': compareExportSets,
  'shm.setMemoryShape': compareSharedShape,
  'shm.reuseKey': compareReuse,
  'shm.copyApiShape': compareCopyApi,
  'shm.keyValidation': compareKeyValidation,
};

function policyFor(id) {
  if (POLICIES[id]) return POLICIES[id];
  if (id.startsWith('shm.length.')) return compareLength;
  return null;
}

function compareCase(id, expectedEntry, actualEntry, paths) {
  const expected = normalizeEntry(expectedEntry, paths);
  const actual = normalizeEntry(actualEntry, paths);
  if (!actual) return { ok: false, diffs: ['no result was produced (process crashed or timed out)'], expected, actual };
  if (expected && expected.status === 'skip') {
    // A skipped oracle case may still be verified structurally by the Linux
    // unit tests; the differential layer has no baseline to compare.
    return { ok: true, skipped: true, reason: expected.reason, expected, actual };
  }

  const policy = policyFor(id);
  if (policy) {
    const result = policy(expected, actual);
    return { ...result, expected, actual };
  }

  const expectedComparable = comparable(expected);
  const actualComparable = comparable(actual);
  if (isDeepStrictEqual(expectedComparable, actualComparable)) {
    return { ok: true, expected: expectedComparable, actual: actualComparable };
  }
  return { ok: false, diffs: diffPaths(expectedComparable, actualComparable), expected: expectedComparable, actual: actualComparable };
}

module.exports = { compareCase, diffPaths, comparable, COMMON_SHARED_EXPORTS };

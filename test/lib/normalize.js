'use strict';

const path = require('node:path');
const { toWindowsPath } = require('./env');

function escapeRegExp(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function buildReplacements(paths) {
  const replacements = [];
  const add = (value, token) => {
    if (!value) return;
    const variants = new Set([path.resolve(value), value]);
    if (process.platform !== 'win32' && /^\//.test(value)) variants.add(toWindowsPath(value));
    for (const variant of variants) {
      replacements.push([variant.replace(/\\/g, '/'), token]);
      replacements.push([variant.replace(/\//g, '\\'), token]);
    }
  };
  add(paths.resources, '<RES>');
  add(paths.bundle, '<BUNDLE>');
  add(paths.addon, '<ADDON>');
  add(paths.shared, '<SHM>');
  add(paths.resultsDir, '<RESULTS>');
  // Longest first so nested paths win over their parents.
  return replacements.sort((a, b) => b[0].length - a[0].length);
}

function normalizeString(value, replacements) {
  let text = value.replace(/\r\n/g, '\n');
  for (const [needle, token] of replacements) {
    if (!needle) continue;
    text = text.replace(new RegExp(escapeRegExp(needle), 'g'), token);
  }
  // Per-run shared-memory keys are random; make them comparable.
  text = text.replace(/\b(?:case|shm)_[A-Za-z0-9_.-]+/g, '<KEY>');
  // Normalize separators after path substitution.
  text = text.replace(/\\/g, '/');
  text = text.replace(/([A-Za-z]):\//g, '$1:/');
  return text;
}

function normalizeValue(value, replacements) {
  if (typeof value === 'string') return normalizeString(value, replacements);
  if (Array.isArray(value)) return value.map(item => normalizeValue(item, replacements));
  if (value && typeof value === 'object') {
    const result = {};
    for (const key of Object.keys(value).sort()) result[key] = normalizeValue(value[key], replacements);
    return result;
  }
  return value;
}

function normalizeEntry(entry, paths) {
  if (!entry) return null;
  const pathSets = Array.isArray(paths) ? paths : [paths || {}];
  const replacements = pathSets.flatMap(set => buildReplacements(set));
  const normalized = normalizeValue(entry, replacements);
  // Runtime details legitimately differ between Wine/Electron and Linux/Node;
  // they are recorded for diagnostics but never compared.
  if (normalized && typeof normalized === 'object') {
    delete normalized.runtime;
    delete normalized.platform;
    if (normalized.error) delete normalized.error.stack;
  }
  return normalized;
}

module.exports = { normalizeEntry, normalizeValue, normalizeString };

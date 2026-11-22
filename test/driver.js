'use strict';

// Runs exactly one observation case inside the target runtime and writes the
// result to a file.  One process per case keeps a native crash from destroying
// the observations that already succeeded.

const fs = require('node:fs');
const path = require('node:path');
const { allCases, byId } = require('./cases');
const { stabilize } = require('./lib/util');

function parseArgs(argv) {
  const result = { _: [] };
  for (let index = 0; index < argv.length; index++) {
    const token = argv[index];
    if (!token.startsWith('--')) { result._.push(token); continue; }
    const name = token.slice(2);
    const next = argv[index + 1];
    if (next === undefined || next.startsWith('--')) { result[name] = true; continue; }
    result[name] = next;
    index++;
  }
  return result;
}

function writeResult(outPath, payload) {
  const directory = path.dirname(outPath);
  fs.mkdirSync(directory, { recursive: true });
  const temporary = `${outPath}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, JSON.stringify(payload));
  fs.renameSync(temporary, outPath);
}

function loadModule(modulePath, label) {
  if (!modulePath) return null;
  try {
    return require(path.resolve(modulePath));
  } catch (error) {
    const wrapped = new Error(`${label} load failed: ${error && error.message || error}`);
    wrapped.cause = error;
    throw wrapped;
  }
}

async function executeCase(options) {
  const id = options.caseId;
  const platform = options.platform || (process.platform === 'win32' ? 'win32' : 'linux');
  const result = { id, platform, runtime: {
    platform: process.platform,
    arch: process.arch,
    node: process.versions.node,
    electron: process.versions.electron || null,
    modules: process.versions.modules,
  } };

  const caseDefinition = byId(id);
  if (!caseDefinition) return { ...result, status: 'unknown-case' };
  if (caseDefinition.engine && !options.allowEngine) {
    return { ...result, status: 'skip', reason: 'engine-case-disabled' };
  }
  if (caseDefinition.engineContext && !options.allowEngineContext && !options.allowEngine) {
    return { ...result, status: 'skip', reason: 'engine-context-required' };
  }

  let shared = null;
  let addon = null;
  try {
    shared = loadModule(options.sharedPath, 'sharedMemory');
    addon = loadModule(options.modulePath, 'skyline addon');
  } catch (error) {
    return {
      ...result,
      status: 'skip',
      reason: 'module-unavailable',
      error: { name: error.name, message: String(error.message || error) },
    };
  }

  if (caseDefinition.group === 'shared-memory' && !shared) {
    return { ...result, status: 'skip', reason: 'shared-memory-unavailable' };
  }
  if (caseDefinition.group !== 'shared-memory' && !addon) {
    return { ...result, status: 'skip', reason: 'skyline-addon-unavailable' };
  }

  // The official loader require()s sharedMemory first and the addon second.
  globalThis.__skyline_addon_keys__ = addon && typeof addon === 'object'
    ? Object.keys(addon).sort()
    : null;
  globalThis.__skyline_addon_loaded__ = !!addon;

  const context = {
    addon,
    shared,
    shell: options.shell || null,
    resources: options.resources || null,
    bundle: options.bundle || null,
    addonPath: options.addonPath || null,
    platform,
    allowEngine: !!options.allowEngine,
  };

  try {
    const value = await caseDefinition.run(context);
    return { ...result, status: 'ok', value: stabilize(value) };
  } catch (error) {
    return {
      ...result,
      status: 'error',
      error: {
        name: error && error.name || typeof error,
        message: String(error && error.message !== undefined ? error.message : error),
        stack: error && error.stack,
      },
    };
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.list) {
    console.log(JSON.stringify(allCases.map(item => ({ id: item.id, group: item.group, engine: !!item.engine }))));
    process.exit(0);
  }

  const id = args.case;
  const out = args.out;
  if (!id || !out) {
    console.error('usage: driver --case <id> --out <file> [--module <addon.node>] [--shared <sharedMemory.node>] [--resources <dir>] [--allow-engine]');
    process.exit(2);
  }

  const payload = await executeCase({
    caseId: id,
    platform: args.platform,
    modulePath: args.module,
    sharedPath: args.shared,
    resources: args.resources || null,
    bundle: args.bundle || null,
    addonPath: args['addon-path'] || null,
    allowEngine: !!args['allow-engine'],
    allowEngineContext: !!args['allow-engine-context'],
  });

  writeResult(out, payload);
  // The native module starts message loops and worker threads; an explicit
  // exit is required, and also makes the wine host terminate deterministically.
  process.exit(0);
}

if (require.main === module) {
  main().catch(error => {
    const args = parseArgs(process.argv.slice(2));
    if (args.out) {
      try {
        writeResult(args.out, {
          id: args.case || null,
          status: 'driver-error',
          error: { name: error && error.name, message: String(error && error.message || error), stack: error && error.stack },
        });
      } catch {}
    }
    process.exit(3);
  });
}

module.exports = { executeCase, writeResult };

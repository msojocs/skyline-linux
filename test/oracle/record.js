'use strict';

// Records the API/validation/shared-memory contract of the official Windows
// module under Wine.  Each case runs in its own process, so a native crash is
// recorded as data instead of aborting the recording.

const fs = require('node:fs');
const path = require('node:path');
const { allCases } = require('../cases');
const { oraclePaths, specPath, resultsDir } = require('../lib/env');
const { runOnce } = require('../lib/runner');

function parseArgs(argv) {
  const args = { engine: false, engineContext: false, only: null, timeout: 30000 };
  for (let index = 0; index < argv.length; index++) {
    const token = argv[index];
    if (token === '--engine') args.engine = true;
    else if (token === '--engine-context') args.engineContext = true;
    else if (token === '--only') args.only = argv[++index];
    else if (token === '--timeout') args.timeout = Number(argv[++index]) || 30000;
  }
  return args;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const paths = oraclePaths();
  if (!paths) {
    console.error('无法找到官方 Windows 模块。请设置：');
    console.error('  SKYLINE_WIN_RESOURCES=<.../resources/app.asar.unpacked>');
    console.error('  SKYLINE_WIN_ELECTRON=<.../electron.exe>');
    process.exit(2);
  }
  for (const [label, file] of Object.entries({ addon: paths.addon, shared: paths.shared, electron: paths.electron, bundle: paths.bundle })) {
    if (!fs.existsSync(file)) {
      console.error(`缺失官方文件 ${label}: ${file}`);
      process.exit(2);
    }
  }

  const selected = allCases.filter(item => {
    if (item.engine && !args.engine) return false;
    if (args.only) return item.id.includes(args.only) || item.group === args.only;
    return true;
  });

  const spec = {
    meta: {
      generatedAt: new Date().toISOString(),
      source: 'official-wechat-devtools-win32-x64',
      devtoolsVersion: process.env.SKYLINE_DEVTOOLS_VERSION || '2.02.2609250',
      electronVersion: '36.6.0',
      engineCases: args.engine,
      engineContextCases: args.engineContext,
    },
    cases: {},
  };

  const counts = { ok: 0, skip: 0, crash: 0, timeout: 0, error: 0 };
  for (let index = 0; index < selected.length; index++) {
    const item = selected[index];
    process.stderr.write(`[${index + 1}/${selected.length}] ${item.id} ... `);
    const result = await runOnce({
      platform: 'win32',
      caseId: item.id,
      paths,
      allowEngine: args.engine,
      allowEngineContext: args.engineContext,
      timeoutMs: args.timeout,
    });
    const status = result.status;
    if (status === 'crash' || status === 'timeout' || status === 'spawn-error') {
      spec.cases[item.id] = { status: status === 'spawn-error' ? 'crash' : status, exitCode: result.exitCode ?? null };
      counts[status === 'spawn-error' ? 'crash' : status]++;
      process.stderr.write(`${status}${result.exitCode !== undefined ? ` (exit ${result.exitCode})` : ''}\n`);
    } else if (status === 'skip') {
      spec.cases[item.id] = { status: 'skip', reason: result.reason };
      counts.skip++;
      process.stderr.write(`skip (${result.reason})\n`);
    } else if (status === 'ok') {
      spec.cases[item.id] = { status: 'ok', value: result.value };
      counts.ok++;
      process.stderr.write('ok\n');
    } else {
      spec.cases[item.id] = { status: 'error', error: result.error };
      counts.error++;
      process.stderr.write(`error (${result.error && result.error.name})\n`);
    }
  }

  fs.mkdirSync(path.dirname(specPath()), { recursive: true });
  const temporary = `${specPath()}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, JSON.stringify(spec, null, 2));
  fs.renameSync(temporary, specPath());

  console.log(JSON.stringify({ spec: specPath(), results: resultsDir(), counts }, null, 2));
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});

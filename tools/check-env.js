'use strict';

// Reports which inputs the test suite can find.  Linux-only runs need the
// shared-memory module; the Windows oracle and cross-platform tests also need
// Wine plus the official DevTools resources.

const fs = require('node:fs');
const path = require('node:path');
const {
  oraclePaths,
  linuxSharedMemoryDefault,
  windowsResources,
  windowsElectron,
  wineBinary,
} = require('../test/lib/env');

const engineDir = path.resolve(__dirname, '..', 'native', 'engine');
const appKernel = path.resolve(__dirname, '..', 'apps', 'empty-page', 'build', 'flutter_assets', 'kernel_blob.bin');
const engine = {
  checkout: fs.existsSync(path.join(engineDir, 'checkout', 'engine', 'src', 'flutter')),
  stage: fs.existsSync(path.join(engineDir, 'out', 'linux-x64-debug', 'libflutter_engine.so')),
  toolchain: fs.existsSync(path.join(engineDir, 'toolchain', 'engine-artifacts', 'icudtl.dat')),
  kernel: fs.existsSync(appKernel),
};

const oracle = oraclePaths();
const shared = linuxSharedMemoryDefault();
const report = {
  linuxSharedMemory: shared,
  linuxSkylineAddon: process.env.SKYLINE_LINUX_SKYLINE_ADDON || '(reference implementation)',
  engine,
  oracle: oracle && {
    resources: oracle.resources,
    electron: oracle.electron,
    addon: fs.existsSync(oracle.addon),
    shared: fs.existsSync(oracle.shared),
    bundle: fs.existsSync(oracle.bundle),
  },
  wine: wineBinary(),
  checks: {},
};

report.checks.l1l2 = true;
report.checks.l3l4 = !!shared;
report.checks.oracle = !!oracle;
report.checks.crossPlatform = !!shared && !!oracle;
report.checks.engine = engine.stage && engine.toolchain && engine.kernel;

console.log(JSON.stringify(report, null, 2));
if (!shared) {
  console.error('未找到 Linux sharedMemory 模块；先运行 npm run native:build，或设置 SKYLINE_LINUX_SHARED_MEMORY。');
  process.exitCode = 1;
} else if (!windowsResources() || !windowsElectron()) {
  console.error('未找到官方 Windows 资源/Electron；仅 L1/L2 与 L3/L4 可运行，跨平台测试会跳过。');
}
if (!report.checks.engine) {
  console.error(
    '引擎冒烟测试所需产物不完整：先运行 npm run toolchain:fetch / app:build，' +
      '再按需运行 engine:fetch / engine:build / native:build。',
  );
}

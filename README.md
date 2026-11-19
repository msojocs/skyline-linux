# skyline-linux

探索把微信开发者工具内置的 Skyline 渲染模块（`skyline.node`）移植到 Linux，并提供
可重复执行的 Windows / Linux 行为一致性测试。

> 当前状态：**可行性结论已明确，L1–L4 测试与阶段 1 API 层已落地；阶段 2 已完成
> 引擎版本固定、构建脚本、kernel 符号清单，并跑通“空页面 → 一帧共享内存”的最小
> 闭环**。阶段 3 第一项已完成：从官方 kernel 恢复 `package:skyline` 源码，补上
> 私有 `dart:ui` 兼容层，并用公开 SDK 重编译 kernel；重编译后的 kernel 已能在
> 公开引擎上启动，并借助 `native/runtime` 的 stub `libskyline.so` 跑通
> `initSkylineForDevtools` → appservice 第一帧。嵌入式运行时（完整 Dart API）已能
> 通过官方 `CreatePageRoute`/`YGConnector`/`NewYGNode`/`NewDecoratedBox`
> 渲染多节点 Skyline 页面（三色条纹），并通过 `NewTextSpan`/`NewText` 渲染文本。渲染引擎的原生 Linux 实现受闭源限制，详见
> [docs/feasibility.md](docs/feasibility.md)。

## 结论先行

官方 `skyline.node` 不是一个轻量的 Node 原生模块，而是：

1. 一个 PE32+ x64 DLL（约 18 MB，638 个具名导出）；
2. 静态链接了微信定制的 Flutter Engine（Dart VM、Skia、Impeller，以及
   `flutter/lib/ui/skyline_ext/*`）；
3. 静态链接了 Skyline 渲染运行时（`skyline/*`，版本 1.4.21）；
4. 通过 Node-API 在宿主进程中注册全局 `SkylineShell` / `SkylineDebugInfo`；
5. 依赖同目录的 `sharedMemory.node` 交换帧数据（共享内存 + 8 字节帧头）。

`skyline/*` 与定制 Flutter Engine 均未开源。经确认，本项目选择 **原生引擎复刻**
路线（2026-11-19）：以公开 Flutter Engine 为底座，按二进制与行为观测复刻
Skyline 运行时与嵌入层。该路线周期长，因此先交付可执行的契约规范与差分测试，
并用 Wine 中的官方引擎作为持续回归基准。完整论证见
[docs/feasibility.md](docs/feasibility.md)，分阶段实施见
[docs/native-roadmap.md](docs/native-roadmap.md)，接口契约见
[docs/contract.md](docs/contract.md)。

## 测试分层

| 层级 | 内容 | Linux 依赖 | Windows 基准 |
| --- | --- | --- | --- |
| L1 静态契约 | 模块导出、全局对象、方法表、属性描述符、arity | 任意目标模块 | Wine + 官方 Electron |
| L2 参数校验 | 每个方法的参数数量/类型/越界错误、窗口生命周期 | 任意目标模块 | Wine + 官方 Electron |
| L3 共享内存 | 外层头、key 校验、零初始化、跨进程可见性、代际替换 | `skyline-shared-memory` | Wine + 官方 `sharedMemory.node` |
| L4 帧协议 | 帧头（frameId/frameSize）、拷贝语义、跨平台读写互通 | `skyline-shared-memory` | Wine 写入 / Linux 读取 |
| L5 引擎渲染 | 确定性页面帧的像素/哈希对比 | 待实现 | 需完整官方 DevTools 环境 |

L1–L4 可在本仓库直接运行；L5 需要完整复刻官方 appservice guest 环境，当前未实现，
受控于 `docs/feasibility.md` 第 3 节的阻塞项。

## 快速开始

```bash
# 1. 构建 Linux 原生模块并运行全部测试（无需 Wine 即可跑 L1–L4）
npm run native:build
npm test

# 1b. 用原生模块对比官方契约（L1/L2）
npm run test:native

# 1c. 阶段 2：轻量 Dart 工具链 + 空页面 kernel（约 250 MB）
SKYLINE_PROXY=http://127.0.0.1:54321 npm run toolchain:fetch
npm run app:build

# 1d. 阶段 2：引擎 checkout/构建（20–30 GB，按需执行）
SKYLINE_PROXY=http://127.0.0.1:54321 npm run engine:fetch
npm run engine:build
npm run native:build
npm run test:engine

# 1e. 阶段 3：dart:ui 兼容层 + package:skyline 重编译
npm run ui:patch                    # 给固定引擎打 dart:ui 兼容补丁并重建 patched SDK
npm run kernel:skyline              # 从官方 kernel 恢复源码并重编译（无需联网）
npm run engine:build                # 让 Application 等类进入引擎核心快照
npm run native:build                # 同时构建 stub libskyline.so
npm run test:kernel && npm run smoke:build && npm run test:ui-compat
npm run test:appservice             # appservice kernel + stub runtime → 第一帧
npm run runtime:embed               # 把运行时嵌入引擎（完整 Dart API）并重建
npm run test:page                   # 渲染多节点 Skyline 页面（三色条纹）

# 2. 记录官方 Windows 模块的契约快照（需要 Wine 11 + Windows Electron）
export SKYLINE_WIN_RESOURCES=/path/to/resources/app.asar.unpacked
export SKYLINE_WIN_ELECTRON=/path/to/electron.exe
npm run spec:record

# 3. 用 Linux 实现对比官方快照
export SKYLINE_LINUX_SKYLINE_ADDON=/path/to/skyline.node     # 可选
export SKYLINE_LINUX_SHARED_MEMORY=/path/to/sharedMemory.node
npm run test:contract
```

本机已有开发者工具安装包时，脚本会自动发现下列默认路径：

```
../wechat-web-devtools-linux/cache/devtools/resources/app.asar.unpacked
../skyline-client-server/cache/electron-win32-x64/electron.exe
../skyline-shared-memory/build/Release/sharedMemory.node
```

测试不依赖网络；仓库不提交任何官方二进制。`npm run spec:record` 产生的
`spec/skyline-node.win32.json` 是唯一提交的基准文件（不含绝对路径）。

### 当前结果（2026-11-19）

```
npm test: 106 tests, 82 pass, 0 fail, 24 skipped
```

- 通过：L1/L2 契约对比（默认目标 `reference/skyline-node.js`；`native/build/skyline.node`
  同样 39/39 通过）、L3/L4 共享内存与帧协议、跨进程可见性、Windows ↔ Linux
  数据区互通、阶段 2 符号清单（73 个 FFI lookup、375 个回调、275 个引擎 native 符号），
  `native/test/engine-smoke.test.js`（引擎启动 → 一帧 128×96 RGBA 写入
  `/dev/shm`，首像素 `[255,0,0,255]`），阶段 3 的
  `test/kernel-rebuild.test.js`（源树/兼容层/重编译 kernel）、
  `native/test/dart-ui-compat-smoke.test.js`（`Application` 运行时行为 + 绿帧）、
  `native/test/skyline-appservice-smoke.test.js`（stub `libskyline.so` 导出
  全部 73 个符号，appservice kernel 启动并输出 320×240 白帧），以及
  `native/test/skyline-page-smoke.test.js`（嵌入式运行时通过官方
  `CreatePageRoute`/`YGConnector`/`NewYGNode`/`NewDecoratedBox` 渲染三色条纹、
  文本页面、带蓝色边框/圆角的 card 页面、JSON 描述符驱动的 row flex 页面，
  嵌套 JSON 树（column 内嵌 row + padding/gap/flex/text）、opacity/clip 节点、
  线性渐变、box shadow、text 样式/对齐（fontWeight/fontStyle/textAlign），以及
  native 定时更新（`DecoratedBoxUpdateColor` → 重绘），以及增量
  `appendChild`/`removeChild`（`YGConnectorUpdateChildren` + relayout），以及
  `updateOpacity`/`updateBorder` 运行时样式更新。
- 跳过：需要完整引擎上下文的 L2 用例（`--engine-context`）与 L5 像素对比。
- 设置 `SKYLINE_LINUX_SKYLINE_ADDON=/path/to/skyline.node` 后，L1/L2 会自动改为
  对比真实 Linux 实现。

### `native/`：Linux 原生 API 层

`native/` 是 Native N-API 模块，已实现 L1/L2 契约（39 项全部通过）：

```bash
npm run native:build
SKYLINE_LINUX_SKYLINE_ADDON=native/build/skyline.node npm run test:contract
```

模块加载时通过内嵌 bootstrap 注册全局 `SkylineShell`/`SkylineDebugInfo`，原生侧
负责回调引用与引擎边界（`native/include/engine.hh`）。当前 `StubEngine` 不含渲染；
下一阶段用 Linux Flutter Engine 后端替换。

### 阶段 2：Engine 版本固定、符号清单与 headless host

- `native/engine/`：固定 Flutter 3.35.3 公开引擎（`REVISION` / `ARTIFACTS.json`），
  提供 `fetch-toolchain.sh`、`fetch-engine.sh`、`build-engine.sh`、
  `build-empty-page.sh`；详见 [native/engine/README.md](native/engine/README.md)。
- `tools/extract-kernel-symbols.js`：直接解析官方 72 MB `kernel_blob.bin` 与
  `isolate_snapshot_data`，生成 `spec/kernel-symbols.json` 和
  [docs/engine-symbols.md](docs/engine-symbols.md)：
  73 个 `DynamicLibrary.lookupFunction` 符号（全部为 `skyline.node` 导出）、375 个
  `stringHash` 回调、591 个 FFI typedef、275 个引擎 `@Native` 符号，以及私有
  `dart:ui` 的 `Application` / `createNativeParagraph` / `warmUpFont`。
- `native/host/`：不依赖 GTK/EGL 的软件渲染 host，启动 engine → 发送 implicit view
  metrics → 处理 vsync → 取一帧 RGBA → 写入 `/dev/shm/skyline_<key>.dat`。
- 本机已于 2026-11-19 完成固定 revision 的 debug engine 构建（357 MB
  `libflutter_engine.so`），`npm run test:engine` 校验一帧 128×96 RGBA、首像素
  `[255,0,0,255]`、`vsyncCount=1`、`presentCount=1`。
- 引擎未构建时 CMake 生成 exit-77 stub，因此 `npm run native:build` 与 `npm test`
  在只有源码的机器上仍可运行。

### 阶段 3：`dart:ui` 兼容层与 package:skyline 重编译

详细说明与限制见 [docs/phase-3-dart-ui-compat.md](docs/phase-3-dart-ui-compat.md)。

- `tools/reconstruct-kernel-sources.js`：解析官方 `kernel_blob.bin` 的
  `uriToSource`，恢复 1951 个源文件（212 个 `package:skyline`、664 个
  `package:flutter`、1072 个其他包）并生成 `package_config.json`；产物写入
  `native/engine/work/`，仓库不提交官方源码。
- `native/engine/dart_ui_compat/`：从官方 `isolate_snapshot_data` 恢复的私有
  `dart:ui` 源码（`application.dart`、`mmtrace.dart`）与增量补丁脚本
  (`apply.py`)；涵盖 `Application`、`WindowStatus`、`createNativeParagraph`、
  `warmUpFont`、`PlatformDispatcher` 的刷新率/GC/阈值/慢栈回调、
  `FramePhase.vsyncTarget`、`TextStyle.lineHeight` 等。
- `native/engine/patch-dart-ui.sh` + `build-skyline-kernel.sh`：给固定引擎打补丁、
  重建 `platform_strong.dill`，再用公开 SDK 编译
  `package:skyline` + `package:sample/devtools.dart`。
- 2026-11-19 实测：重编译 kernel 80.8 MB，SDK hash `a29e08c72e`；公开引擎不再报
  `Invalid SDK hash`，可执行到 `package:sample/devtools.dart main`。
  `apps/dart-ui-compat-smoke` 在重建后的引擎上验证兼容层运行时行为并输出绿帧。
- `native/runtime/skyline_runtime_stub.cc`：Linux 版运行时，导出
  `spec/kernel-symbols.json` 中的全部 73 个符号，实现
  `InitDartAPIDLSkyline` / `AllocateFfiTable` / `Register` /
  `ShareRuntimeFromRuntimeShell` / `CreateWindow` 等引导路径。
  - 独立 `native/build/libskyline.so` 使用 Dart API DL，只做引导；
  - `npm run runtime:embed` 把同一份源码以 `SKYLINE_EMBEDDED` 编译进
    `libflutter_engine.so`，从而获得完整 Dart API；运行时在 UI task runner 上
    调用官方 `CreatePageRoute`/`YGConnector`/`NewYGNode`/`NewDecoratedBox`，
    构造 root YGNode + 3 个子 YGNode（3 条竖排色带），由
    `YogaCalculateLayout` 填充每个节点的布局缓冲，渲染出 320×240 三色页面；
    `SKYLINE_STUB_PAGE=text` 时改用 `NewTextSpan`/`NewText`，由 Flutter 文本
    引擎绘制 “Skyline on Linux”；`=card` 时在文本外再包一层带 3px 蓝色边框、
    16px 圆角的 `DecoratedBox`；`SKYLINE_STUB_PAGE='{...}'`（或 `_JSON` 环境
    变量/`@file`）由 JSON 描述符驱动嵌套 row/column flex 布局（padding/gap/
    flex/size/文本），支持 `opacity`/`clip`/`gradient`/`shadow` 与文本样式
    （fontWeight/fontStyle/letterSpacing/lineHeight/textAlign/maxLines）；
    `npm run test:page` 覆盖十种页面。
- 未完成：真实 appservice/V8 节点树输入、完整 yoga 属性、绘制（边框/文本/图片）、
  手势与平台通道。

### `reference/skyline-node.js`

仓库内的参考实现是可执行的接口规范：它精确复刻官方模块的全局对象、方法顺序、
属性描述符、参数校验与错误消息，但不包含渲染引擎（`createWindow` 会明确报错）。
它用于：

1. 让差分测试在没有 Linux 引擎时也能验证 L1/L2；
2. 作为原生 N-API 实现或 Wine 代理实现的 API 层蓝本；
3. 证明测试框架能捕获真实差异（开发过程中它先后暴露了方法顺序、arity、setter
   多余参数、dispatch arity 等 5 类不一致）。

### 已知差异与待办

1. key 字符集：Linux 有意收紧（防路径穿越），测试按“安全 key 一致、危险 key
   必须拒绝”判定。
2. L5 引擎渲染对比：需先解决最小宿主中的
   `node::NodePlatform::ForIsolate` 断言（见 `docs/feasibility.md` 3）。
3. 引擎后端按“原生引擎复刻”路线推进：阶段 1 的 Native N-API API 层已完成；阶段 2
   已固定公开 Flutter 3.35.3 引擎、产出符号清单并落地 headless host 骨架；阶段 3
   已完成私有 `dart:ui` 兼容层、`package:skyline` 重编译、stub/嵌入式运行时，并
   通过官方 `CreatePageRoute`/`YGNode`/`DecoratedBox` 渲染第一张 Skyline 页面。
   下一步是接入 appservice/V8 节点树并补齐布局、绘制、手势与平台通道。
4. 共享内存缺失 key / 返回值语义已于 2026-11-19 对齐 Windows
   （`skyline-shared-memory`）。

## 目录

```
docs/            可行性分析、接口与协议契约、生成式符号清单、阶段 3 兼容层说明
spec/            由 Windows 官方模块录制的 JSON 快照 + kernel 符号清单
reference/       可执行接口规范（无引擎），原生实现的 API 层蓝本
native/          Native N-API 模块（API 层已完成，引擎为 stub）
native/runtime/   阶段 3：stub Linux libskyline.so（73 个导出符号）
native/engine/   阶段 2/3：引擎版本固定、fetch/build 脚本、dart:ui 兼容补丁
native/host/     阶段 2：headless 软件渲染 host
apps/empty-page/ 阶段 2：最小纯 dart:ui 页面（编译为 kernel_blob.bin）
apps/dart-ui-compat-smoke/ 阶段 3：兼容层运行时 smoke（Application + 绿帧）
tools/           环境检查、kernel 符号提取器、kernel 源树恢复工具
test/cases/      Windows / Linux 共用的观察用例
test/lib/        归一化、差分比较、Wine/Linux 运行器、帧协议实现
test/fixtures/   跨平台共享内存读写夹具
test/oracle/     Wine 侧契约录制脚本
```

## 与现有项目的关系

- [`msojocs/skyline-shared-memory`](https://github.com/msojocs/skyline-shared-memory)：本仓库的
  Linux `sharedMemory.node` 实现，L3/L4 直接以它为被测目标。
- [`msojocs/skyline-client-server`](https://github.com/msojocs/skyline-client-server)：现有
  Wine 服务端方案；本仓库的差分测试可作为其回归基线。

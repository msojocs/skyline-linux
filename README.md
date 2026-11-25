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
> 渲染多节点 Skyline 页面（三色条纹），并通过 `NewTextSpan`/`NewText` 渲染文本。
> 阶段 3 的节点输入已从手写 JSON 描述符切换为**官方渲染层 JS bundle 的真实节点
> 操作流**：`tools/record-skyline-ops.js` 在 Node V8 中运行官方
> `app.js`（exparser + skyline-js）并抓取 `createElement`/`appendChild`/
> `setAttribute`/`appendStyleSheet`/`attach` 等操作，嵌入式运行时按 CSS 级联与
> flexbox 子集回放，再调用真实 `package:skyline` 回调物化。目前已覆盖滚动
> （触摸/鼠标拖拽、滚轮）、`list-view`/`grid-view` 与 builder 增量 mount、
> 图片原生绘制与 `objectFit`/网络图片、HTTP/路由栈/键盘/触摸/鼠标/日志平台
> 通道。路由栈回放支持 `notifyNavigateTo`/`notifySwitchTab`/`notifyNavigateBack`
> 与多条 `CreatePageRoute`；宿主 `--key-script` 经 `FlutterEngineSendKeyEvent`
> 和 `skyline/keyboard` 平台消息驱动焦点文本更新。本轮已补齐阶段 3 的边界项：
> CSS `transition` 与 `addAnimatedStyle` 时间线、builder 二次变更回调、
> `position: fixed` 视口定位、3D/skew/perspective transform、图片
> `repeat`/`background-image`/SVG/网络缓存与取消、输入法选区/光标/组合输入、
> CJK 字体回退，以及 L5 Linux 基线/容差比较 harness。后续增量补齐：
> `transform-origin`、圆角/gradient 增量更新、root 样式 targeted update、
> fixed 节点提升到页面根连接器（滚动祖先内保持固定）、录制器
> `--animate-ms` 驱动 JS worklet/derived value 采样、`align-self`/`order`/按轴
> gap/`flex-wrap`/`align-content`、`flex-basis`/`flex-shrink`、逐边 border、
> `overflow: hidden` 裁剪、逐角圆角、资源路径缓存与
> `background-position`/`object-position`、`box-sizing: content-box`、百分比 padding/margin、`position: sticky`、
> `radial-gradient`/`conic-gradient`、显式 `background-size` 长度值、
> `transform-origin` 的 z 分量（3D 枢轴）、内存 pending 字体与 CSS `font-family`。
> L5 已接入官方 Windows
> 引擎的真实帧基线
> （`spec/l5-baseline.win32.json`，Wine 渲染进程 + dpr=2 采集）。
> 阶段 3 任务清单与 x/y 进度见 [docs/phase-3-tasks.md](docs/phase-3-tasks.md)。
> 渲染引擎的原生 Linux 实现受闭源限制，详见
> [docs/feasibility.md](docs/feasibility.md) 与
> [docs/appservice-ops.md](docs/appservice-ops.md)。

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
| L3 共享内存 | 外层头、key 校验、零初始化、跨进程可见性、代际替换 | `native/shared-memory`（内置） | Wine + 官方 `sharedMemory.node` |
| L4 帧协议 | 帧头（frameId/frameSize）、拷贝语义、跨平台读写互通 | `native/shared-memory`（内置） | Wine 写入 / Linux 读取 |
| L5 引擎渲染 | 确定性页面帧的像素/哈希对比 | `skyline_host` + `spec/l5-baseline.json` | Wine + 官方 Windows Electron 渲染进程 |

L1–L4 可在本仓库直接运行；L5 已有 Linux 基线与**官方 Windows 引擎真实帧**基线
（`npm run test:l5`）。`tools/l5-windows.js` 在 Wine 渲染进程中以 dpr=2 启动官方
`skyline.node`，经 `notifyAppLaunch` + `PageContext` 回放夹具并从共享内存读取帧
（Windows 原始帧为 BGRA，工具转为 RGBA），结果写入
`spec/l5-baseline.win32.json`。25 个夹具中 14 个逐像素完全一致，
`shadow`/`radial-conic`/`background-size` 最大差 1，`overflow` 仅有边缘级差异；
`transform3d`/`transform-origin-z` 及 D2/D6 后的 `vertical-align`/
`text-overflow`/`white-space` 为已记录的文本/布局残余差异（inline-block、
nowrap/normal、aspect-ratio 已对齐），`map` 逐像素一致，`gridview`/`listview`
不可比；详见 `docs/feasibility.md` §3.1。

汇总缺口报告：`npm run gaps`（`tools/conformance-gaps.js`）把 Windows API oracle
覆盖率、L5 逐像素结果、官方符号清单、原生运行时未接线的回调，以及
`awesome-skyline/examples` 里用到但运行时未解析的 CSS/内置标签汇总成 Markdown。
候选回调的逐项分类与探针状态见 `docs/callback-coverage.md`。
报告中的“未解析”是候选缺口，是否与官方 Windows 行为不同仍需用 L5 探针夹具确认。

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
npm run test:host-engine               # skyline.node -> 引擎 -> 共享内存帧 -> 回调
npm run test:host-engine:aot           # release skyline.node 自动 AOT
npm run test:shm-frame                 # setMemory/getMemory 数据区帧桥接
npm run test:shared-memory             # 内置 sharedMemory.node 的 API 回归
npm run test:engine:resize             # --resize-script 视口尺寸更新
# 1d-2. 可选：release/AOT
npm run engine:build -- --runtime-mode release
npm run app:aot                        # apps/empty-page/build/libapp.so
cmake -S native -B native/build-release \
  -DSKYLINE_ENGINE_ROOT="$PWD/native/engine/out/linux-x64-release"
cmake --build native/build-release
npm run test:engine:aot
npm run test:host-engine:aot
# Skyline appservice AOT（需先 npm run kernel:skyline）
npm run runtime:embed -- --runtime-mode release
bash native/engine/build-aot.sh --app-dir native/engine/work/skyline-app \
  --entry native/engine/work/skyline-app/packages/sample/lib/devtools.dart --patched-sdk
npm run test:skyline:aot

# 1e. 阶段 3：dart:ui 兼容层 + package:skyline 重编译
npm run ui:patch                    # 给固定引擎打 dart:ui 兼容补丁并重建 patched SDK
npm run kernel:skyline              # 从官方 kernel 恢复源码并重编译（无需联网）
npm run engine:build                # 让 Application 等类进入引擎核心快照
npm run native:build                # 同时构建 stub libskyline.so
npm run test:kernel && npm run smoke:build && npm run test:ui-compat
npm run test:appservice             # appservice kernel + stub runtime → 第一帧
npm run runtime:embed               # 把运行时嵌入引擎（完整 Dart API）并重建
npm run test:page                   # 渲染多节点 Skyline 页面（三色条纹）
npm run ops:record                  # 录制官方渲染层 bundle 的节点操作流
npm run test:ops                    # 回放节点操作流（CSS/flexbox/滚动）
npm run ops:record -- --tap-option 1  # 录制真实 tap 触发的增量操作
npm run ops:record -- --tap-text "Jump To PageB"  # 录制 SPA 路由切换
npm run test:ops                     # 含路由栈/builder/objectFit/网络图片/键盘
npm run l5:baseline                  # 生成 Linux L5 帧哈希基线
npm run l5:windows                   # Wine 官方引擎采集 spec/l5-baseline.win32.json
npm run test:l5                      # Linux 基线 + Windows 逐像素容差比较

# 1f. 部署到微信开发者工具（复制文件后直接启动，无需环境变量）
#     默认部署 release 引擎 + AOT 快照：debug 引擎以 -O0/_DEBUG/GLIBCXX_DEBUG
#     编译软件光栅化，模拟器分辨率下滚动只有 ~25fps；release 引擎的引擎侧
#     帧率可以稳定在 ~60fps。AOT 快照与 release 引擎 VM 配置绑定，修改 Dart
#     运行时源码后需要重新生成。
#     需要 patchelf 在 PATH 中（apt install patchelf 或 pip install patchelf）。
bash native/engine/patch-skyline-runtime.sh --runtime-mode release
npm run devtools:aot                 # native/engine/work/skyline-app/build/libapp.so
npm run native:build:release          # native/build-release/skyline.node
npm run devtools:stage
../wechat-web-devtools-linux/bin/wechat-devtools
# 脚本会写入 skyline.node、libflutter_engine.so、libskyline.so、icudtl.dat、
# bundle/libapp.so 和 flutter_assets（不含 JIT kernel_blob.bin），以及
# sharedMemory.node；Dart 入口参数由 addon 根据 skyline-addon 路径自动识别。
# 需要 JIT kernel（改 Dart 代码后免 AOT 的快速迭代）时：
npm run devtools:stage:debug         # 对应 npm run native:build / engine:build / kernel:skyline
npm run test:l5:windows              # 仅比较已提交的 Windows 基线

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
native/build/sharedMemory.node
```

共享内存模块已内置在 `native/shared-memory/`（源自 `skyline-shared-memory`），
`npm run native:build` 会同时产出 `native/build/sharedMemory.node`；仍可用
`SKYLINE_LINUX_SHARED_MEMORY` 指向其他构建。

测试不依赖网络；仓库不提交任何官方二进制。`npm run spec:record` 产生的
`spec/skyline-node.win32.json` 与 `npm run l5:windows` 产生的
`spec/l5-baseline.win32.json` 是提交的基准文件（不含绝对路径）。

### 当前结果（2026-11-22）

```
npm test: 198 tests, 174 pass, 0 fail, 24 skipped
npm run test:ops: 75 tests, 75 pass
npm run test:l5: 2 tests, 2 pass (Linux 确定性基线 + Windows 逐像素容差比较)
```

- 通过：L1/L2 契约对比（默认目标 `reference/skyline-node.js`；`native/build/skyline.node`
  同样 53/53 通过，含 14 个引擎上下文用例；10 个官方进程崩溃用例跳过）、L3/L4 共享内存与帧协议、跨进程可见性、Windows ↔ Linux
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
  `updateOpacity`/`updateBorder` 运行时样式更新，以及
  `native/test/skyline-appservice-ops.test.js`（官方 exparser bundle 节点操作流 ->
  CSS 级联 + flexbox 布局 -> 真实 `package:skyline` 回调；attach 后结构重建与
  `TextSpanUpdate*`/`DecoratedBoxUpdate*` 细粒度更新；真实 tap -> `setData` ->
  操作流 -> 重绘；`@font-face` -> `LoadResourceAsync` -> `NotifyResourceLoad`
  字体资源通道；`scrollview` 渲染 + `ScrollViewUpdateScrollTop`；触摸/鼠标拖拽与
  滚轮滚动（`ShouldGestureResponse`/`ShouldAcceptGesture` 手势协商）；
  `list-view` sliver 与 `grid-view` aligned 网格；8×8 BMP 经
  `NewDirectRenderWidget` 原生绘制；`ForwardHttpGet` -> Dart `HttpClient` ->
  `OnForwardHttpGetResponse` HTTP 通道；`HandleTouchEvent`/`HandlePageRouteDone`
  路由与输入通道；宿主 `--pointer-script` 指针注入命中 YGNode 树）。
- 跳过：需要完整引擎上下文的 L2 用例（`--engine-context`）。L5 的 Windows 基线已提交，
  `npm run test:l5` 会直接执行逐像素比较。
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
- `native/host/host_engine.cc`：把同一引擎接入 `skyline.node`。`createWindow`
  为每个 window 启动独立 platform thread（`FlutterEngineRun` + vsync baton），
  出帧后 BGRA→RGBA 写入调用方传入的共享内存数据区（6 号参数 ArrayBuffer，或
  9 号参数 16 字节 ptr+size 描述符），再经 N-API threadsafe function 调用
  `onWindowRasterized`。支持多窗口、destroy/重建；未构建引擎时 CMake 仍回退到
  `StubEngine`。
- 本机已于 2026-11-19 完成固定 revision 的 debug engine 构建（357 MB
  `libflutter_engine.so`），`npm run test:engine` 校验一帧 128×96 RGBA、首像素
  `[255,0,0,255]`、`vsyncCount=1`、`presentCount=1`；
  `npm run test:host-engine` 用 `skyline.node` + `apps/empty-page` 校验
  createWindow → 共享内存帧 → 回调、多窗口与窗口重建。
- 引擎未构建时 CMake 生成 exit-77 stub，因此 `npm run native:build` 与 `npm test`
  在只有源码的机器上仍可运行。

### 阶段 3：`dart:ui` 兼容层与 package:skyline 重编译

详细说明与限制见 [docs/phase-3-dart-ui-compat.md](docs/phase-3-dart-ui-compat.md)。

- `tools/reconstruct-kernel-sources.js`：解析官方 `kernel_blob.bin` 的
  `uriToSource`，恢复 1951 个源文件（212 个 `package:skyline`、664 个
  `package:flutter`、1072 个其他包）并生成 `package_config.json`；产物写入
  `native/engine/work/`，仓库不提交官方源码。
- `native/engine/dart_ui_compat/`：从官方 `isolate_snapshot_data` 恢复的私有
  `dart:ui` 源码（`application.dart`、`mmtrace.dart`、`skyline_ext.dart`）与
  `native/engine/patches/` 中的 unified diff 补丁；涵盖 `Application`、
  `WindowStatus`、`createNativeParagraph`、
  `warmUpFont`、`PlatformDispatcher` 的刷新率/GC/阈值/慢栈回调、
  `FramePhase.vsyncTarget`、`TextStyle.lineHeight` 及私有文本编码标志等。
- `native/engine/patch-dart-ui.sh` + `build-skyline-kernel.sh`：给固定引擎打补丁、
  重建 `platform_strong.dill`，再用公开 SDK 编译
  `package:skyline` + `package:sample/devtools.dart`。
- 2026-11-19 实测：重编译 kernel 80.8 MB，SDK hash `a29e08c72e`；公开引擎不再报
  `Invalid SDK hash`，可执行到 `package:sample/devtools.dart main`。
  `apps/dart-ui-compat-smoke` 在重建后的引擎上验证兼容层运行时行为并输出绿帧。
- `native/runtime/`：Linux 版运行时（入口 `skyline_runtime_stub.cc` + `appservice_*`
  回放模块），导出
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
- 已接入：官方渲染层 bundle 的真实节点操作流（录制 + 回放），CSS 级联子集、
  flexbox 子集布局、文本/边框/背景/透明度绘制、attach 后结构重建 + 文本/样式
  细粒度更新、tap 手势驱动的 `setData` 更新、字体资源加载平台通道、scroll-view
  渲染与程序化滚动、触摸/鼠标拖拽与滚轮（含 `concreteWidgetType` 命中测试修复）、
  `list-view`/`grid-view` sliver、`NewDirectRenderWidget` 图片原生绘制，以及
  HTTP/路由/触摸/鼠标/日志平台通道。阶段 3 边界项已补齐：CSS `transition` 与
  `addAnimatedStyle` 时间线、builder 二次变更回调、`position: fixed`、
  `transform` 3D/skew/perspective、图片 `repeat`/`background-image`/SVG/缓存与
  取消、输入法选区/光标/组合输入、CJK 字体回退与 L5 Linux 基线/harness。
  后续增量：`transform-origin`、逐角圆角/gradient 增量更新、root 样式 targeted
  update、fixed 节点提升到页面根连接器、录制器 JS worklet/derived value 采样、
  `align-self`/`order`/按轴 gap/`flex-wrap`/`align-content`、`flex-basis`
  （px/%/auto）与 `flex-shrink` 缩放收缩、逐边 border、
  `overflow: hidden` 裁剪、逐角圆角与资源路径缓存、`background-position` /
  `object-position`、`box-sizing: content-box`、百分比 padding/margin、`position: sticky`、
  `radial-gradient`/`conic-gradient`（尺寸关键字、`at` 位置与硬停止）、
  显式 `background-size` 长度/百分比值、`transform-origin` 的 z 分量、
  内存 pending 字体（`AddPendingFont`/`RemovePendingFont` + `font-family`），
  以及私有 `dart:ui` 文本编码
  标志（`backgroundUseLineMetrics`/`useCoreTextWordBreak`，按官方位布局写入
  native 并在 SkParagraph 中应用行框背景/选区）、native view 通道
  （`<texture>` → `NewTexture`/`TextureDispose`，指针经
  `HandleNativeViewPointerEvent` 转发；Linux 使用占位 slot）。
- 未完成：完整 yoga 属性；`transform3d` 与官方引擎的保真差异；grid/list 合成 op 流
  触发官方 native builder 回调；native view 的真实原生内容。

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
2. L5 引擎渲染对比：官方 Windows 帧已在 `spec/l5-baseline.win32.json` 中提交，
   Linux 侧按 dpr=2 重放并逐像素比较。已知差异：`transform3d`（3D 变换保真）、
   `gridview`/`listview`（合成 op 流不触发官方 builder 回调），见
   `docs/feasibility.md` §3.1。
3. 引擎后端按“原生引擎复刻”路线推进：阶段 1 的 Native N-API API 层已完成；阶段 2
   已固定公开 Flutter 3.35.3 引擎、产出符号清单，并把 headless engine 接入
   `skyline.node`（每窗口独立 platform thread、多窗口、共享内存出帧与
   `onWindowRasterized` 回调）；阶段 3 已完成私有 `dart:ui` 兼容层、
   `package:skyline` 重编译、stub/嵌入式运行时，并覆盖 appservice/V8 真实操作流、
   增量更新、滚动/手势、list/grid 与 builder（含二次变更）、图片（`objectFit`/
   网络/`repeat`/`background-image`/SVG/缓存/取消）、路由栈、键盘、HTTP/输入
   平台通道、CSS `transition`/`addAnimatedStyle`、`position: fixed` 与
   `transform` 3D/skew/perspective、输入法选区/光标/组合输入，以及 L5 Linux
   基线/harness。阶段 2 已同时支持 JIT 与 release/AOT
   （`libapp.so` + `FlutterEngineCreateAOTData`）；剩余项见
   [docs/native-roadmap.md](docs/native-roadmap.md)（viewport resize、
   Skyline appservice 的 AOT 帧级验证、Windows L5 guest）。
4. 共享内存缺失 key / 返回值语义已于 2026-11-19 对齐 Windows
   （内置 `native/shared-memory`，源自 `skyline-shared-memory`）。

## 目录

```
docs/            可行性分析、接口与协议契约、生成式符号清单、阶段 3 兼容层与操作流说明
docs/phase-3-tasks.md  阶段 3 任务清单与 x/y 进度（后续更新以此为准）
spec/            由 Windows 官方模块录制的 JSON 快照 + kernel 符号清单
reference/       可执行接口规范（无引擎），原生实现的 API 层蓝本
native/          Native N-API 模块（API 层已完成，引擎为 stub）
native/runtime/   阶段 3：stub Linux libskyline.so（73 个导出符号）
native/engine/   阶段 2/3：引擎版本固定、fetch/build 脚本、dart:ui 兼容补丁
native/host/     阶段 2：headless 软件渲染 host
apps/empty-page/ 阶段 2：最小纯 dart:ui 页面（编译为 kernel_blob.bin）
apps/dart-ui-compat-smoke/ 阶段 3：兼容层运行时 smoke（Application + 绿帧）
tools/           环境检查、kernel 符号提取器、kernel 源树恢复、节点操作流录制、
                 L5 Linux/Windows 基线采集与比较
test/cases/      Windows / Linux 共用的观察用例
test/lib/        归一化、差分比较、Wine/Linux 运行器、帧协议实现
test/fixtures/   跨平台共享内存读写夹具、appservice 操作流夹具
test/oracle/     Wine 侧契约录制脚本
```

## 与现有项目的关系

- [`msojocs/skyline-shared-memory`](https://github.com/msojocs/skyline-shared-memory)：本仓库
  Linux `sharedMemory.node` 实现的上游来源；现已内置到 `native/shared-memory/`
  （移除 vcpkg/spdlog 与外部 node_modules 依赖），L3/L4 直接以内置构建为被测目标。
- [`msojocs/skyline-client-server`](https://github.com/msojocs/skyline-client-server)：现有
  Wine 服务端方案；本仓库的差分测试可作为其回归基线。

## CI 与发布

`.github/workflows/release.yml` 在两种事件下构建 Linux 产物：

- push 到 `master`：更新滚动预发布 `preview`（prerelease，tag 始终指向最新提交），
  资产为 `skyline-linux-preview-<sha>-linux-x64.tar.gz` 与 `.sha256`；
- push `v*` tag：创建/更新同名 Release（自动生成 release notes），资产为
  `skyline-linux-<tag>-linux-x64.tar.gz` 与 `.sha256`。

CI 在 `ubuntu-latest` 上执行 `npm run native:build` 和 `npm test`，然后打包
`skyline.node`、`sharedMemory.node`、stub `libskyline.so` 与 `BUILD-INFO.txt`。
为控制时长与体积，CI 不拉取 20–30 GB 的 Flutter 引擎，也不生成 AOT 快照
（依赖引擎的契约/渲染用例会自动跳过）；完整的开发者工具部署仍需在本地执行
`engine:build` / `app:aot` / `devtools:stage`。

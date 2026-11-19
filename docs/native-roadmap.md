# 原生引擎复刻路线（阶段 2–4）

阶段 1（Native N-API API 层）已完成，见 `native/`。本文记录后续阶段的实施顺序、
可验证产物与风险，避免把“复刻引擎”当成一次性任务。

## 阶段 2：Linux Flutter Engine 宿主

### 进度（2026-11-19）

已落地：

- **版本固定**：`native/engine/REVISION` 固定公开 Flutter 3.35.3 / Dart 3.9.2 /
  monorepo commit `ddf47dd3…`；`ARTIFACTS.json` 记录下载 URL 与 SHA-256。官方私有
  引擎 rev `6b917cf8…` 仍不可获取，仅作溯源记录。
- **构建脚本**：`fetch-engine.sh`（depot_tools + gclient sync，可 `--check`）、
  `build-engine.sh`（`gn` + `ninja flutter_engine`，产出 `libflutter_engine.so` /
  `flutter_embedder.h`）、`fetch-toolchain.sh`（Dart SDK + patched SDK +
  `icudtl.dat`，约 250 MB）、`build-empty-page.sh`（纯 dart:ui 空页面 →
  `kernel_blob.bin`）。脚本透传 `SKYLINE_PROXY`，并用 `uv` 选择 Python 3.12。
- **符号清单**：`tools/extract-kernel-symbols.js` 解析官方 kernel 与
  `isolate_snapshot_data`，产物 `spec/kernel-symbols.json`、
  `docs/engine-symbols.md`：
  - 73 个 `DynamicLibrary.lookupFunction` 符号，全部存在于 `skyline.node` 的 PE
    导出表；
  - 375 个通过 `stringHash(name)` 注册的 Dart 回调；
  - 591 个 FFI typedef、21 个 `Struct` 布局；
  - 275 个引擎 `@Native` 符号（`Canvas::*`、`Paragraph*::*`、`SceneBuilder::*`、
    `PlatformConfigurationNativeApi::*`、`flutter_fluency_tracker::*` 等）；
  - 私有 `dart:ui` 增补：`Application` 类（`dart:ui/application.dart`）、
    `createNativeParagraph()`、`warmUpFont()`。
- **宿主骨架**：`native/host/` 使用软件渲染 `FlutterSoftwareRendererConfig`，不依赖
  GTK/EGL；按 implicit view metrics + vsync baton + `surface_present_callback`
  取一帧，写入 `/dev/shm/skyline_<key>.dat`（16 字节外层头 + 8 字节帧头 + RGBA）。
  `native/test/engine-smoke.test.js` 校验帧头、尺寸与已知红色像素。
- **引擎构建与最小闭环（2026-11-19 已完成）**：`engine:fetch` 拉到固定 revision
  (`ddf47dd3…`)，`engine:build` 产出 357 MB `libflutter_engine.so` + header +
  `icudtl.dat`；`npm run test:engine` 启动引擎、渲染一帧并校验通过：
  `width=128, height=96, frameSize=49152, firstPixel=[255,0,0,255]`，
  `vsyncCount=1, presentCount=1`，共享内存外层头 `dataSize=196616`。
- **宿主形态**：阶段 2 先采用独立可执行文件 `skyline_host` 验证引擎生命周期与帧
  输出，不把 `libflutter_engine.so` 链接进 Node 进程；因此不会触碰
  `node::NodePlatform::ForIsolate`。后续再决定是进程内嵌入还是由 N-API 模块拉起
  host 子进程。

待执行：

- ~~官方 kernel 的 SDK hash 为 `294a28fe17`，公开 3.35.3 patched SDK 为
  `a29e08c72e`，证实官方 Dart SDK 是私有分支；阶段 3 需按符号清单为
  `Application` / `createNativeParagraph` 等补丁 `dart:ui` stub~~（已完成，见
  “阶段 3”进度；空页面仍使用公开 SDK 单独编译）。
- ~~实测（2026-11-19）：直接用公开引擎加载官方 `kernel_blob.bin` 失败：
  `Dart Error: Can't load Kernel binary: Invalid SDK hash.`。因此不能只给引擎补
  stub 就运行官方 kernel；阶段 3 需要基于公开 SDK 重新编译 kernel（或提供与
  `294a28fe17` 匹配的 patched SDK 与 dart:ui 声明）~~（已完成：重新编译后的
  kernel 使用公开 hash，并已在重建后的公开引擎上启动）。

### 原始目标与步骤

目标：让 Dart 应用（`kernel_blob.bin` / 重新生成的 AOT 快照）在 Linux 上跑起来，
先不追求 Skyline 语义，只验证引擎生命周期与帧输出。

1. **固定 Engine 版本**
   - 官方 `flutter_engine_git_rev = 6b917cf8…` 在公开仓库中不可见（定制分支）。
   - 选择公开 stable 版本并记录在 `native/engine/REVISION`；所有 AOT/快照必须由同一
     版本生成。
2. **构建 Linux Engine**
   - 使用 depot_tools 构建 `flutter_embedder` + `libflutter_engine.so`（x64）。
   - 产物进入 `native/engine/out/`，不提交二进制。
3. **应用加载策略（二选一）**
   - JIT：直接加载 70 MB `kernel_blob.bin`，便于调试；需要 debug/JIT 引擎。
   - AOT：用同版本 `gen_snapshot` 对 kernel 生成 Linux `libapp.so`；需要先解析
     `dart:ui` skyline_ext 的符号，否则链接阶段会被裁剪/报错。
4. **宿主接入**
   - 在 `native/src/` 增加 `FlutterEngine` 实现 `skyline::Engine`：
     `FlutterEngineRun` + 自定义 `FlutterProjectArgs`（assets、AOT）。
     - headless：EGL surfaceless 或 Flutter software renderer；
     - 每帧 RGBA 拷贝到共享内存（数据区 8 字节帧头 + 像素）。
   - **不要**复用 Electron/Node 的 `node::NodePlatform` 来托管 Skyline 的 V8
     isolate；官方最小宿主正是在此处触发
     `NodePlatform::ForIsolate` 断言。独立 V8/平台更可控。
5. **验收**
   - `native/test/engine-smoke.test.js`：启动引擎 → 输出一帧 → 校验帧头。
   - 该阶段不开启 L5 像素对比。

## 阶段 3：Skyline 运行时复刻

### 进度（2026-11-19）：阶段 3 第一项已落地

- **源码恢复**：`tools/reconstruct-kernel-sources.js` 从官方 kernel 的
  `uriToSource` 恢复 1951 个源文件（212 个 `package:skyline`、664 个
  `package:flutter`、其余为 pub 依赖），生成 `package_config.json`；根库为
  `package:sample/devtools.dart`。产物写入 `native/engine/work/`，不提交。
- **dart:ui 兼容层**：从官方 `isolate_snapshot_data` 恢复私有
  `application.dart` / `mmtrace.dart` 及平台补丁，落在
  `native/engine/dart_ui_compat/`；`patch-dart-ui.sh` 把新 part 与增量补丁
  （`apply.py`）应用到固定引擎，重建 `platform_strong.dill`。
  覆盖 `Application`、`WindowStatus`、`createNativeParagraph`、`warmUpFont`、
  `PlatformDispatcher` 的刷新率/GC/阈值/慢栈、`FramePhase.vsyncTarget`、
  `TextStyle.lineHeight` 等；详见
  [phase-3-dart-ui-compat.md](phase-3-dart-ui-compat.md)。
- **重编译 kernel**：`npm run kernel:skyline` 用公开 SDK + 兼容层编译出
  80.8 MB kernel，SDK hash `a29e08c72e`，73/375/591/21 项符号清单与官方一致。
- **运行时验证**：重建引擎核心快照后，公开引擎加载重编译 kernel 不再报
  `Invalid SDK hash`，执行到 `package:sample/devtools.dart main`；
  `apps/dart-ui-compat-smoke` + `npm run test:ui-compat` 验证
  `Application.put/find/get`、`WindowStatus`、`FramePhase.vsyncTarget` 等运行时行为，
  并输出一帧 64×48 纯绿 RGBA。
- **stub Linux 运行时（2026-11-19）**：`native/runtime/skyline_runtime_stub.cc`
  构建 `libskyline.so`，导出官方清单中的全部 73 个符号，实现
  `InitDartAPIDLSkyline`、`AllocateFfiTable`/`Register`、
  `ShareRuntimeFromRuntimeShell`、`CreateWindow` 等引导路径。宿主新增
  `--entrypoint-arg`/`--no-shutdown`；`npm run test:appservice` 已跑通
  `initSkylineForDevtools` → `runApp`，输出 320×240 第一帧。
- **嵌入式运行时与第一张 Skyline 页面（2026-11-19）**：
  `npm run runtime:embed` 将同一份运行时以完整 Dart API 编译进
  `libflutter_engine.so`，导出 73 个符号；在 UI task runner 上通过官方
  `NewDecoratedBox` / `NewYGNode` / `YGConnector` / `CreatePageRoute`
  构造 root + 3 个子节点的页面，`YogaCalculateLayout` 为每个节点写入
  9-float 布局缓冲；`NewTextSpan`/`NewText` 已能通过 Flutter 文本引擎渲染文本，
  `NewDecoratedBox` 的边框/圆角也已生效；JSON 描述符可驱动**递归嵌套**的
  row/column flex（每层 padding/gap/flex/size）与 view/text 节点。
  `opacity` / `clip` / `gradient` / `shadow` 节点也已接入（`NewOpacity` /
  `NewClipRRect` / `NewLinearGradient` / `NewBoxShadow`）。`npm run test:page`
  覆盖条纹、文本、卡片、JSON row、嵌套树、opacity、clip、渐变、阴影、文本
  样式/对齐、native 定时颜色/透明度/边框更新，以及 `appendChild`/`removeChild`
  增量布局十四种场景。
- **下一步**：接入 appservice/V8 节点树（或用测试协议模拟），替换合成页面；
  逐渐补全 yoga 属性、边框/文本/图片绘制、手势与资源加载。

按依赖顺序推进，每一块都以 Windows 官方输出为基准：

1. **WXML/样式输入**：从 appservice 侧拿到节点树与计算样式（已由 `float-pigment`
   等模块提供 CSS 解析，可先复用官方 JS 侧输入）。
2. **布局（yoga）**：复刻 `skyline/layout/*` 的约束求解；用快照对比
   `layout_paragraph` 等输出。
3. **绘制**：`skyline/paint/*`、`SkylineNormalImageImpl`、Impeller/Skia 快照；
   先做纯色/边框/圆角/文本/图片的最小集合。
4. **运行时**：`skyline/runtime/engine/devtools_v8_engine.cc` 的 V8 宿主与 FFI 表
   （`InitDartAPIDLSkyline` 等）；优先实现 appservice 启动与路由。
5. **平台通道**：资源加载、HTTP、路由、日志、亮度、触摸/键盘/滚轮（契约见
   `docs/contract.md` 第 3、5 节）。
6. **验收**：每个子系统一个差分测试；帧级用 `test/lib/frame.js` 的 Reader 解析后
   做哈希/像素容差。

## 阶段 4：接入共享内存与 L5

- 把阶段 2 的帧缓冲写入 `skyline-shared-memory`（Linux `/dev/shm` 文件格式）。
- 打开 L5：确定性页面（固定尺寸、字体、图片）在 Windows 与 Linux 各渲染一帧，
  比较 SHA-256；允许按像素容差（抗锯齿/字体栅格化差异）放宽。
- 目标顺序：全等 → 容差 → 稳定容差。

## 风险与判定点

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 引擎版本与 AOT 快照不匹配 | Dart 应用无法启动 | 固定版本；实测公开引擎加载官方 kernel 报 `Invalid SDK hash`，阶段 3 需重新编译 kernel 或提供匹配的 patched SDK |
| skyline_ext 符号缺失 | 引擎删减/链接失败 | 阶段 2 已产出 73 个 FFI lookup、375 个回调、275 个 `@Native` 符号；阶段 3 按清单生成 stub |
| 布局/绘制细节不可完全复刻 | L5 无法全等 | 容差 + 子集优先，文档记录差异 |
| V8 宿主与 Electron 冲突 | 崩溃（已观测） | 独立 V8 平台，不依赖 NodePlatform |
| 工程周期 | 以人年计 | 里程碑交付；每阶段都保持 L1–L4 绿 |

## 当前下一步（建议）

1. ~~产出 `native/engine/REVISION` 与 Engine 构建脚本~~（已完成）；
2. ~~从 `kernel_blob.bin` 提取 `skyline_ext`/FFI 符号清单~~（已完成，
   `spec/kernel-symbols.json` / `docs/engine-symbols.md`）；
3. ~~先跑通“空 Flutter 页面 → 一帧共享内存”的宿主与冒烟测试~~（已完成：
   `engine:fetch` + `engine:build` + `test:engine` 于 2026-11-19 通过）；
4. ~~用符号清单为私有 `dart:ui`（`Application`、`createNativeParagraph`、
   `warmUpFont` 等）生成兼容层~~（已完成：从官方 snapshot 恢复源码，
   `npm run ui:patch` + `npm run kernel:skyline`；重编译 kernel 已在公开引擎启动）；
5. ~~实现 Linux 侧的 `skyline.node` 引导路径，让 `initSkylineForDevtools` 越过
   `DynamicLibrary.open`~~（已完成：`native/runtime` stub `libskyline.so`，
   `npm run test:appservice` 输出第一帧）；
6. ~~让 `CreateWindow` 后的 Skyline 页面渲染出来~~（已完成：嵌入式运行时 +
   官方 `CreatePageRoute`/`YGNode`/`DecoratedBox` 合成页面）；
7. **下一步**：接入 appservice/V8 节点树，补齐 yoga 属性与边框/文本/图片绘制、
   手势、资源与平台通道，并逐步对齐 `lineHeight`、`clipOp` 等 no-op 兼容项。

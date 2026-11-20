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
- **宿主形态（2026-11-20 更新）**：阶段 2 早期采用独立可执行文件
  `skyline_host` 验证引擎生命周期与帧输出；现已选择**进程内接入**：
  `native/host/host_engine.cc` 编译进 `skyline.node`（引擎存在时），为每个窗口
  启动独立 platform thread，直接写调用方共享内存并回调
  `onWindowRasterized`。不链接 Node 的 V8/NodePlatform，因此不触碰
  `node::NodePlatform::ForIsolate`；`skyline_host` 仍保留为离线/回放工具。
- **N-API 引擎后端（2026-11-20）**：`skyline.node` 在
  `native/engine/out/linux-x64-debug` 存在时链接 `libflutter_engine.so`
  （否则回退 `StubEngine`）。`createWindow` 解析 bundle/icu、校验
  width/height/dpr、启动引擎并发送 implicit view metrics；present 回调把
  BGRA 转 RGBA 写入 6 号参数（ArrayBuffer/typed array）或 9 号参数
  （16 字节 ptr+size 描述符）指向的数据区，再经 N-API threadsafe function
  调用 `onWindowRasterized`。支持多窗口、destroy/重建、首帧早于回调注册时补发。
  `npm run test:host-engine` 覆盖单窗口、多窗口、窗口重建、dpr=2 物理像素与
  缺失 kernel。

- **AOT / release（2026-11-20）**：`native/engine/build-aot.sh` 用
  `frontend_server --aot --tfa` 重编译 kernel，再由与 release 引擎同构建的
  `gen_snapshot` 生成 `libapp.so`。宿主与 N-API 后端通过
  `FlutterEngineCreateAOTData` + `FlutterProjectArgs.aot_data` 加载；
  `kernel_blob.bin` 缺失时自动切 AOT，两者都在时可用 `--aot-library` /
  `SKYLINE_AOT_LIBRARY` 强制。已构建 `linux-x64-release` 引擎并跑通
  `npm run test:engine:aot`（128×96 红帧）与 `npm run test:host-engine:aot`
  （release `skyline.node` 在无 `kernel_blob.bin` 的 bundle 上自动 AOT）。
  注意：工具链下载的 gen_snapshot 是
  JIT-release 构建，与 product 引擎的 snapshot 配置不匹配，脚本优先使用
  `out/host_release/gen_snapshot`。
- **Skyline appservice AOT（2026-11-20）**：
  `patch-skyline-runtime.sh --runtime-mode release` 把嵌入式运行时装入 release
  引擎（73 个导出 + `libskyline.so`）；`build-aot.sh --patched-sdk` 生成
  `package:skyline` 的 `libapp.so`（9.4 MB）。`npm run test:skyline:aot` 在
  release 引擎上以 AOT 启动 appservice，回放 stripes 操作流并校验三条色带
  （[255,51,102] / [51,204,102] / [51,102,255]）。

- **共享内存帧桥接（2026-11-20）**：`createWindow` 的 6 号参数直接接受 Linux
  `skyline-shared-memory` 的 external ArrayBuffer（数据区）。新增
  `npm run test:shm-frame`：`setMemory(key, bytes)` → `createWindow(...)` →
  `onWindowRasterized` → `getMemory(key)` 观察同一份 `frameId/frameSize/RGBA`
  （empty-page 红帧）。16 字节外层头仍由 `skyline-shared-memory` 管理；
  Windows 风格的 9 号 `ptr+size` descriptor 也继续支持。

- **viewport resize（2026-11-20）**：独立 host 新增 `--resize-script`
  （`<ms> <width> <height> [dpr]`），按时间发送
  `FlutterEngineSendWindowMetricsEvent`；`npm run test:engine:resize` 验证
  128×96 → 64×64 后最终帧为 16384 字节红帧。N-API 契约没有 resize 方法，暂不
  对外暴露。

待执行：

- **N-API 多 view / 运行时 resize**：契约没有对应 JS 方法；目前每个窗口仍固定
  一个 implicit view，standalone host 的 resize 路径已验证。
- **引擎指标导出**：已记录 window ready/first frame/destroy/stopped，启动 15 s
  超时与同步参数错误；尚未提供崩溃转储或性能指标接口（契约没有对应 JS 方法）。
  N-API 帧回调队列为 1，Node 繁忙时合并突发通知。

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

> 任务清单与 x/y 进度见 [phase-3-tasks.md](phase-3-tasks.md)。

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
- **appservice/V8 节点操作流（2026-11-19）**：新增
  `tools/record-skyline-ops.js`，在 Node V8 中运行官方渲染层 bundle
  (`skyline-addon/bundle/flutter_assets/assets/app.js`)，用 capture shim 抓取
  exparser custom backend 的 `createElement`/`setText`/`setAttribute`/
  `setStyle`/`addClass`/`appendChild`/`splice`/`appendStyleSheet`/`attach`/
  `startRender` 操作流。`native/runtime` 新增操作回放：节点表、CSS 级联子集
  （类/元素/id 选择器、内联样式、文本继承）与 flexbox 子集布局，最后通过真实
  `NewYGNode`/`NewDecoratedBox`/`NewText`/`CreatePageRoute` 物化。
  `npm run test:ops` 覆盖提交的条纹操作夹具与官方 bundle 现场录制页
  （`#ccc` 操作栏、白色按钮、文本、绿色 rect），并断言确定性帧 SHA-256。
  录制器还支持 `--tap-option N`：在页面 attach 后向 exparser 注入真实 `tap`
  事件，捕获 `setData` 产生的 `updates` 批次（`setStyle`/`addClass` 等）；
  回放侧按 `afterMs` 投递，经 `YGConnectorUpdateChildren` +
  `MarkYGNodeNeedsLayout` 重绘（tap 后按钮变黄）。
  详见 [appservice-ops.md](appservice-ops.md)。
- **增量更新（2026-11-19）**：`updates` 批次已区分结构变更与样式/文本变更：
  结构变更经 `YGConnectorUpdateChildren` 重建根子树；样式/文本走
  `DecoratedBoxUpdateColor/Border`、`OpacityUpdateOpacity`、
  `TextSpanUpdateText/TextColor/FontSize/...`、`TextApplyUpdate` 细粒度回调，
  并重新布局。布局写入改为每节点 acquire/write/release，避免 Dart GC 移动
  typed data 后使用失效指针。
- **资源加载平台通道（2026-11-19）**：实现 `LoadResourceSync/Async` 与
  `NotifyResourceLoad` 回调；CSS `@font-face` 经 `LoadFontFace` ->
  `ResourceLoader.loadResourceAsync` -> `LoadResourceAsync` -> `NotifyResourceLoad`
  -> `FontLoader` -> `NotifyFontFaceLoad` 全链路跑通。资源根路径由
  `SKYLINE_RESOURCE_ROOT` 指定，异步回调投递到 UI task runner。
- **scroll-view 与输入（2026-11-19）**：`name == "scrollview"` 的节点调用真实
  `NewScrollView`，子节点以 `isRoot` YGNode 进入 Flutter ListView；`setScrollTop`
  经 `ScrollViewUpdateScrollTop` 更新并以新初始偏移重建。宿主新增
  `--pointer-script`，通过 `FlutterEngineSendPointerEvent` 注入触摸事件。
- **命中测试与手势协商（2026-11-19）**：修正 `NewYGNode` 的
  `concreteWidgetType`/`yogaConnector` 参数——带 YGConnector 的容器必须报告
  `kYGConnector` 并传入 connector 句柄，否则 `YGConnectorRenderObject.hitTest`
  只返回 `size.contains()`，整棵子树的 RenderPointerListener 不在命中路径上。
  修复后 `shouldAcceptUserOffset` 为真，`SkylineVerticalDragGestureRecognizer`
  经 `ShouldGestureResponse`/`ShouldAcceptGesture` 胜出，触摸拖拽、鼠标拖拽与
  滚轮滚动均生效。
- **list/grid（2026-11-19）**：`listview`/`gridview` 节点调用真实
  `NewListView`/`NewGridView`；位于 scroll-view 内时作为 sliver 直接挂到
  `CustomScrollView`，独立时补一层 custom scroll view。item 以 origin 重新布局，
  grid aligned 按列宽重新测量。`listbuilder`/`gridbuilder` 容器展开。
- **图片原生绘制（2026-11-19）**：`image` 节点读取 `src`/`url`，Skia 解码后以
  `DlImageGPU` 包装，经 `NewDirectRenderWidget` 的原生 paint 回调把 Dart
  `Canvas` 解包为引擎 `flutter::Canvas`，用 `DisplayListBuilder::DrawImageRect`
  直接绘制。
- **HTTP / 路由 / 输入通道（2026-11-19）**：`ForwardHttpGet` 委托 Dart
  `RequestHttpResource`（`dart:io HttpClient`），完成后经
  `OnForwardHttpGetResponse` 回到 UI task runner；`HandlePageRoute(Done)`、
  `HandleTouchEvent`/`HandleMouseEvent`、`NotifyUserGestureInProgressChange` 已
  解析并记录；日志导出保持实现。
- **路由栈回放（2026-11-19）**：`notifyNavigateTo`/`notifySwitchTab`/
  `notifyNavigateBack` 操作分别映射到 `CreatePageRoute`（openType
  kAppLaunch/navigateTo/switchTab）与 Dart 注册的 `SwitchTab`、
  `NavigateBack`；每个 `attach` 物化一条路由。`tools/record-skyline-ops.js`
  新增 `--tap-text`（按元素文本注入 tap，兼容旧 bundle 的
  `registerEventCallback`），可录制 `app_0.js` SPA 的真实
  `Jump To PageB` 两页操作流。夹具 `appservice-route.ops.json` 覆盖 push +
  pop；官方 bundle 录制用例覆盖 `didPush pageId: 1`。
- **键盘输入（2026-11-19）**：宿主新增 `--key-script`，同时发送
  `FlutterEngineSendKeyEvent` 与 `skyline/keyboard` 平台消息；重编译的
  appservice kernel 在 UI 线程把消息交给嵌入运行时新增导出
  `DispatchSkylineKeyEvent`，运行时向焦点节点追加文本并走
  `TextSpanUpdate*` 刷新。`focusInput`/`keyboardStatus` 操作覆盖焦点与
  `UpdateKeyboardStatus`（native → Dart）。
- **图片 objectFit / 网络图片（2026-11-19）**：`mode`/`objectFit`/CSS
  `object-fit` 支持 fill/contain/cover/none/scale-down/widthFix（cover 用
  `clipRect` 裁剪，widthFix 在测量阶段按解码宽高比改高度）；`http(s)://`
  图片经 `RequestHttpResource` 异步获取，响应在 UI runner 上解码并用
  `YGConnectorUpdateChildren` 重建页面。
- **builder 增量 mount（2026-11-19）**：`listbuilder`/`gridbuilder` 调用真实
  `NewListBuilder`/`NewGridBuilder`，`onBuildChild`/`onDisposeChild` 回调把
  index 投递到 UI runner，再按需物化 captured item 并调用
  `NewListBuilderItem`/`ListBuilderAppendChild`；100 个声明 child 的夹具初始
  只 mount 5 个可视项。
- **兼容项（2026-11-19）**：`Canvas.clipRRect(ClipOp.difference)` 在
  dart:ui 兼容层用 `Path.combine(difference)` + `clipPath` 实现（不再按
  intersect 处理）；`TextStyle/ParagraphStyle.lineHeight` 仍受公开引擎文本
  编码限制，保留记录。
- **absolute / transform（2026-11-20）**：CSS `position: absolute|fixed` 从 flex
  流中移除绝对定位节点，支持 `top/right/bottom/left`（px/%）与 left+right /
  top+bottom 拉伸；`transform` 支持 `translate/translate3d`、`scale/scale3d`、
  `rotate`、`rotateX/rotateY`、`skewX/skewY`、`perspective`、`matrix/matrix3d`，
  经 `YGNodeSetupTransform` + `YGNodeUpdateTransformOriginAndAlignment`
  （origin 中心 + alignment topLeft）绕元素中心绘制，不改变布局。测试夹具
  `appservice-absolute.ops.json`、`appservice-transform.ops.json`、
  `appservice-transform3d.ops.json`。CSS `box-shadow` 首层经 `NewBoxShadow`
  接入 `NewDecoratedBox`，增量更新走 `DecoratedBoxUpdateShadow`；
  夹具 `appservice-shadow.ops.json`。
- **二次变更与动画（2026-11-20）**：CSS `transition`/`transition-*` 解析并回放
  （16ms 时间线、target/override、opacity/background/color/border/transform）；
  `addAnimatedStyle` 支持 `timeline` 样式帧；`position:fixed` 按视口定位；
  builder 元数据/item/index 与 `UpdateChildren`/`InsertBefore`/`RemoveChild`
  细粒度回调，builder-only 批次不再整页重建；图片 `repeat`、CSS
  `background-image`、Dart `SvgToRgba` SVG 解码、URL 缓存与
  `CancelRequestHttpResource`；输入光标/选区/组合输入与 `InputHoldKeyboard`
  触发；CJK `fontFamilyFallback`。`npm run test:ops` 覆盖到 54 个用例。
- **L5 harness（2026-11-20）**：`test/lib/l5.js` 提供 SHA-256 与逐通道容差比较，
  `tools/l5-baseline.js --write` 生成 `spec/l5-baseline.json`；
  `tools/l5-windows.js` 在 Wine + 官方 Windows Electron 的渲染进程中以 dpr=2
  采集真实 Windows 帧，`spec/l5-baseline.win32.json` 已提交。`npm run test:l5`
  同时校验 Linux 基线与 Windows ↔ Linux 逐像素容差比较（含 `ignoreRects`）。
  真实采集方法与差异清单见 `docs/feasibility.md` §3.1。
- **后续增量（2026-11-20）**：CSS `transform-origin`（px/%/关键字）、
  `DecoratedBoxUpdateBorderSideRadius` 逐角圆角（含百分比）、CSS
  linear-gradient 与 `DecoratedBoxUpdateGradient`、root 自身样式 targeted
  update、fixed 节点提升到页面根连接器（滚动祖先内保持固定）、录制器
  `--animate-ms` 驱动 JS worklet mapper/derived value 并采样为
  `addAnimatedStyle` 时间线；flexbox 增加 `align-self`、`order`、按轴 gap、
  `flex-wrap`/`align-content`、`flex-basis`（px/%/auto）与按
  `flex-shrink * base` 缩放的溢出收缩，逐边 border 与 `overflow: hidden`
  裁剪；资源加载按路径缓存（≤4 MB）；`background-position` / `object-position`
  （1/2/4 值关键字、%、px 与边距形式）。
- **下一步**：`transform3d` 保真对齐、native view 真实内容接入（当前 Linux
  为占位 slot），以及继续补全 yoga 属性。

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
7. ~~接入 appservice/V8 节点树~~（已完成：官方渲染层 bundle 录制 + 回放，
   `npm run ops:record` / `npm run test:ops`）；
8. ~~增量节点/样式更新、scroll/list/grid、手势协商、图片原生绘制、HTTP/输入
   通道~~（已完成，见阶段 3 进度）；
9. ~~路由栈回放、键盘输入、图片 `objectFit`/网络图片、list/grid builder
   增量 mount~~（已完成，见阶段 3 进度；`clipOp` 也已对齐，
   `lineHeight` 仍受引擎文本编码限制）；
10. ~~`transition` 动画、builder item 二次变更回调、输入法选区/光标~~（已完成，
    见阶段 3 进度）；~~L5 Linux 基线/harness~~（已完成）；~~Windows 官方引擎的
    真实 L5 采集~~（已完成，见 `docs/feasibility.md` §3.1）。剩余：滚动祖先内的
    fixed、私有 `dart:ui` 文本编码标志、JS worklet 动态采样。

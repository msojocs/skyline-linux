# 阶段 3：`dart:ui` 兼容层与 package:skyline 重编译

> 2026-11-19 完成。目标是把官方 `kernel_blob.bin` 里编译进去的
> `package:skyline`（以及 `package:flutter` 等依赖）用**公开 Dart 3.9.2 SDK**
> 重新编译，并在公开 Flutter 引擎上启动，从而绕开私有 SDK hash
> `294a28fe17` 与公开 `a29e08c72e` 不匹配、且无法加载官方 kernel 的阻塞。

## 1. 关键发现：官方产物自带完整源码

官方 72 MB `kernel_blob.bin` 的每个 component 都带 `uriToSource`，共
1951 个源文件：

| 来源 | 数量 | 说明 |
| --- | --- | --- |
| `package:skyline/*` | 212 | 完整的 Skyline Flutter 应用 |
| `package:flutter/*` | 664 | 含腾讯/微信补丁的框架源码 |
| 其他 pub 包 | 1072 | win32、rxdart、petitparser、xml、sqflite… |
| `dart:core` 补丁 | 2 | `map.dart` / `map_patch.dart`（由 SDK 提供） |
| 生成的 plugin registrant | 1 | 桌面/移动插件注册，Linux 阶段可忽略 |

根库是 `package:sample/devtools.dart`：

```dart
void main(List<String> args) {
  initSkylineForDevtools(
    id: int.parse(args[0]),
    runtimeHolderPtr: int.parse(args[1]),
    windowWidth: double.parse(args[2]),
    windowHeight: double.parse(args[3]),
    createRuntime: int.parse(args[4]) == 1,
    skylineAddonPath: args[5],
  );
}
```

另外，官方 `isolate_snapshot_data` 内嵌了私有 `dart:ui` 的源码片段
（`org-dartlang-sdk:///flutter/lib/ui/*.dart`），包括
`application.dart`、`mmtrace.dart` 以及各文件相对公开版的 diff。这使得兼容层
可以按“官方原始实现”落地，而不是靠猜测 API。

## 2. 已恢复的私有 `dart:ui` 表面

| 成员 | 来源 | 兼容实现 |
| --- | --- | --- |
| `Application`（`current`/`top`/`fromId`/`isAlive`/`put`/`find`/`get`/`id`/`zone`/`window`/`platformDispatcher`/`channelBuffers`/`isDefaultApplication`/`_exit`） | `dart:ui/application.dart` | 逐行恢复；`platformDispatcher` 指向公开引擎的单例，`top` 在栈空时惰性创建默认应用，避免官方实现的无限递归 |
| `WindowStatus` + `FlutterView.windowStatus` | `window.dart` | 固定返回 `normal`（公开引擎无窗口状态通道） |
| `createNativeParagraph()` | `painting.dart` | 返回未初始化的 `_NativeParagraph`，由 Skyline FFI 回调填充，与官方一致 |
| `warmUpFont(String)` | `painting.dart` | 官方为 `@Native('FontWarmUp::WarmUp')`；公开引擎无此符号，暂为 no-op |
| `entryPoint` / `neverInline` / `MMTrace` / `MMFluencyTracker` | `mmtrace.dart` | 原样恢复；native 调用仅在 `enable` 为 true 时发生 |
| `PlatformDispatcher.getDisplayRefreshRate()` | `platform_dispatcher.dart` | 返回 60.0 Hz |
| `collectGarbage()` / `setUiRunnerThreshold()` / `setRasterRunnerThreshold()` | 同上 | no-op（ANR/GC hook 未移植） |
| `scheduleFrameIgnoreVsync()` | 同上 | 退化为 `scheduleFrame()` |
| `recordStartupTiming()` | 同上 | no-op |
| `onReportSlowRunnerStack` | 同上 | 可赋值但不会触发（无 native 上报） |
| `isUseParallelPipeline()` / `setUseParallelPipeline()` | 同上 | Dart 静态状态，默认 false |
| `FramePhase.vsyncTarget` / `compositeFrameStart` | 同上 | 枚举追加；`FrameTiming` 把二者分别映射到 `vsyncStart` / `buildStart`，native 负载仍为公开引擎的 11 个 int |
| `TextStyle.lineHeight` / `ignoreClusterLetterSpacing` / `useCoreTextWordBreak` | `text.dart` | 按官方位布局写入 native 编码（bit 20/21/22），`lineHeight` 另经 `ParagraphBuilder::Create`/`pushStyle` 传入 |
| `ParagraphStyle.lineHeight` / `ignoreClusterLetterSpacing` / `backgroundUseLineMetrics` | 同上 | 按官方位布局写入 native 编码（bit 13/14/15）；`backgroundUseLineMetrics` 由 SkParagraph 应用 |
| `Canvas.clipRRect(..., clipOp:)` | `painting.dart` | 接受参数；`difference` 用 `Path.combine` 模拟，`intersect` 走原生快速路径 |
| `SceneBuilder.setCheckerboard*()` | `compositing.dart` | no-op 扩展方法 |
| `Platform.isOhos` | Dart SDK `io/platform.dart` | 追加 `operatingSystem == "ohos"`，与官方私有 SDK 一致 |

### 2.1 私有文本编码与 native 落地

官方 `isolate_snapshot_data` 中的私有 `dart:ui/text.dart` 给出了完整位布局：

| 编码 | bit | 含义 | native 处理 |
| --- | ---: | --- | --- |
| `TextStyle._encoded` | 20 | `lineHeight`（px，单独传参） | `txt::TextStyle.line_height`；有字号时换算为 SkParagraph 的 height 倍数 |
|  | 21 | `ignoreClusterLetterSpacing` | 写入 SkParagraph `TextStyle` |
|  | 22 | `useCoreTextWordBreak` | 写入 SkParagraph `TextStyle`，供 CoreText 分词平台使用 |
| `ParagraphStyle._encoded` | 13 | `lineHeight`（px，单独传参） | 默认 `txt::TextStyle.line_height` |
|  | 14 | `ignoreClusterLetterSpacing` | 默认 `txt::TextStyle` |
|  | 15 | `backgroundUseLineMetrics` | `skia::textlayout::ParagraphStyle`；span 背景与选区使用完整行框 |

`_NativeParagraphBuilder._constructor` / `_pushStyle` 的 `@Native` 签名同步增加
`double lineHeight`，与官方一致；`flutter/lib/ui/text/paragraph_builder.cc`
解码后写入 `txt` 样式，`flutter/txt/src/skia/paragraph_builder_skia.cc` 再映射到
SkParagraph。`backgroundUseLineMetrics` 在 `TextLine::paintBackground` 与
`getRectsForRange` 中生效。`useCoreTextWordBreak` 仅在 CoreText 分词实现中有
行为差异；Linux 与官方 Windows 一样继续使用 ICU line breaker。

`npm run test:ui-compat` 的 smoke 页现在构造两段 40px strut + 10px run 的文本：
开启 `backgroundUseLineMetrics` 时蓝色 span 背景覆盖整行框，关闭时只覆盖该 run
的字体度量区域；同一段落同时设置 `useCoreTextWordBreak` 与
`ignoreClusterLetterSpacing`，用于验证编码路径。

兼容层刻意**不**采用私有引擎对 embedder 接口的改动（`_addView`、`render`、
`updateSemantics` 等增加 `applicationId` 参数），因为这些 Dart hook 的签名必须与
公开 C++ 引擎一致。

## 3. stub Linux 运行时与 appservice 第一帧

官方 `package:skyline` 在 Linux 上按名称打开 `libskyline.so`（`skylineAddonPath`
仅在 Windows 分支使用）。本项目的 `app_compat/apply.py` 给开发者工具版本加了
补丁：Linux DevTools 分支直接 `DynamicLibrary.open(skylineAddonPath)`，打开已经
加载的 `skyline.node` 并通过其依赖 `libflutter_engine.so` 解析 73 个符号，因此
DevTools 包不再需要 `libskyline.so`。`native/runtime/`（入口
`skyline_runtime_stub.cc` + 按 `sources.txt` 拆分的 `appservice_*` 模块）提供
ABI 兼容的 stub：

- 从 `lookupFunction<Native, Dart>` 出现处提取 73 个符号的真实返回类型
  （`Void` / `IntPtr` / `Handle` / `Bool` / `Int32` / `Double` / 结构体），
  参数列表在不需要时用可变参数忽略（SysV x64 下 ABI 兼容）；
- `InitDartAPIDLSkyline` 调用 `Dart_InitializeApiDL` 并返回 0；
- `AllocateFfiTable` / `Register` 接受 375 个 Dart 回调的 `stringHash` 注册
  （追加表，暂不回调）；
- `CreateRuntimeHolder` / `ShareRuntimeFromRuntimeShell` / `CreateWindow` /
  `CreateEmptyWindow` 返回非空假指针，满足 Dart 侧 `assert(ptr != 0)`；
- `NotifyFlutterReady` 从 FFI 表按 `stringHash("RunApplication")` 找到回调并同步
  调用，因此在 `createRuntime=1`（DevTools 实际路径）下也会执行
  `runApplication()`；宿主传入的 holder 若不是本库创建的指针，则使用 fallback
  holder，避免解引用无效地址；
- `GetFeatureToggle` 全部返回 false，日志函数输出到 stderr；
- 资源加载、HTTP 转发、路由/触摸/鼠标通道、native view
  （`<texture>`/`HandleNativeViewPointerEvent`）、图片原生绘制与滚动/list/grid
  已在嵌入式运行时实现；`LayoutParagraph*`、`YogaTriggerIntersection*`、键盘
  等导出仍返回零值或 no-op（见
  [appservice-ops.md](appservice-ops.md) 第 6 节）。

宿主新增 `--entrypoint-arg`（可重复）把参数传给 `devtools.dart main`，以及
`--no-shutdown` 用于只取第一帧后立即退出（appservice 的异步工作会让
`FlutterEngineShutdown` 变慢，且必须在平台线程调用）。

实测：

```bash
npm run native:build
npm run test:appservice
```

- stub 导出 73/73 个符号；
- `initSkylineForDevtools` → `SkylineApplicationDevtools` → FFI 初始化 →
  `ShareRuntimeFromRuntimeShell` → `CreateWindow` → `runApp` 全部完成；
- 输出 320×240 RGBA 第一帧（无 splash cache 时为白色），
  `vsyncCount=1`、`presentCount=1`。

### 3.1 嵌入式运行时（完整 Dart API）与第一张 Skyline 页面

独立 `libskyline.so` 只能使用 Dart API DL；DL 不暴露 `Dart_NewTypedData` /
`Dart_TypedDataAcquireData` / 字符串创建，无法构造 `NewYGNode` 需要的
`Float32List` 与 Widget 对象，也无法在 native→Dart callback 中安全地创建对象。
因此增加嵌入式变体：

- `native/engine/patch-skyline-runtime.sh` 按 `sources.txt` 将 `native/runtime`
  的源码以 `SKYLINE_EMBEDDED` 复制进
  `flutter/shell/platform/embedder/skyline_runtime/`，在 `BUILD.gn` 的
  `embedder` target 中编译，并把 73 个符号加入
  `embedder_exports.lst`；重建后 `libflutter_engine.so` 直接导出这些符号。非
  DevTools 的独立运行时仍在同目录创建 `libskyline.so` 符号链接；DevTools 版本
  由上述补丁直接打开 `skyline.node`。
- 页面构造在 `NotifyFlutterReady` 之后投递到 UI task runner：
  不能在 FFI callback 内同步再次调用 native callback（VM 会报
  `Cannot invoke native callback when API callbacks are prohibited`）。
- task 中 `Dart_EnterIsolate` + `Dart_EnterScope`，创建空字符串与 9 元素
  `Float32List`，然后调用官方构造器：
  - 3 个 `NewDecoratedBox(ptr, color, ...)`（`#FF3366` / `#33CC66` /
    `#3366FF`）；
  - 3 个 `NewYGNode(..., isRoot=false, child=decoratedBox)`，返回
    `YGNodeParentDataWidget`；
  - `Dart_NewList` + `Dart_ListSetAt` 组成 children，交给
    `NewYGConnector(enableStackingPaint: false, children)`；
  - 根 `NewYGNode(..., isRoot=true, child=connector)`；
  - `CreatePageRoute(..., root=rootYGNode, config)` 将页面推入 Navigator。
- `Dart_TypedDataAcquireData` 会开启 VM 的 “no callback” scope，取到指针后必须
  立即 `Dart_TypedDataReleaseData`；`YGNode` 持有该 `Float32List`，内存不会移动，
  后续 `YogaCalculateLayout(nodeAddress, maxW, maxH)` 直接写入 9 个 float。
- 合成页面由 `SKYLINE_STUB_PAGE=1` 显式开启（默认不注入，避免改变正常
  appservice 启动行为）。
- `YogaCalculateLayout` 在布局模型中为 root 和 3 个子节点分别写入
  `top/left/width/height`，形成 3 条 80px 高的竖排色带（240/3）。
- 结果：`npm run test:page` 输出 320×240 帧：顶部 `#FF3366`、中部 `#33CC66`、
  底部 `#3366FF`，`vsyncCount=2`、`presentCount=1`。
- 文本模式 `SKYLINE_STUB_PAGE=text`：`NewTextSpan` + `NewText`
  （`enableNativeParagraph=false`）走 Flutter 文本引擎，输出白底黑字
  “Skyline on Linux”；测试断言字形包围盒在 20px inset 内且暗色像素 > 300。
- 卡片模式 `SKYLINE_STUB_PAGE=card`：文本外再包 `NewDecoratedBox`
  （`#F5F5F5` 填充、3px `#3366FF` 实线边框、16px 圆角），验证边框/圆角绘制、
  内边距与文本叠加；测试断言边框、填充、外部背景像素和字形。
- JSON 描述符模式：`SKYLINE_STUB_PAGE='{"direction":"row","padding":10,
  "gap":10,"children":[...]}'`（或 `SKYLINE_STUB_PAGE_JSON`、`@file`）解析
  `direction`/`padding`/`gap` 与每个 child 的 `type`（view/text）、`color`、
  `borderColor`/`borderWidth`/`radius`、`flex`/`size`。子节点可递归嵌套，
  每层独立 row/column、padding、gap；`LayoutJsonNode` 递归计算，子节点坐标写入
  父节点局部坐标系（与 `YGConnector`/`boxParentData.offset` 的约定一致）。
  测试覆盖 1:2 flex + 90px 文本列、column 内嵌 row（pink 容器 + 绿色 flex
  子节点 + 白色文本 + 60px 蓝色子节点）、`opacity`（0.5 红色叠加白底）、
  `clip`（40px 圆角裁剪）、`gradient`（角度 + colors/stops 线性渐变）和
  `shadow`（NewBoxShadow：颜色/offset/blur/spread/inset）节点类型；text 支持
  `fontWeight`/`fontStyle`/`letterSpacing`/`lineHeight`/`textAlign`/`maxLines`。
- 定时更新：JSON 节点可带 `updateColor`/`updateAfterMs`；native 在 UI task
  runner 上 `PostDelayedTask`，用 `Dart_NewPersistentHandle` 保存 DecoratedBox
  句柄，到点后 `Dart_HandleFromPersistent` 并调用
  `DecoratedBoxUpdateColor`，触发 Flutter 重绘。宿主 `--frames 0` 表示持续捕获
  到超时、写最后一帧；测试验证 300ms 后红→蓝。
- 增量节点操作：顶层 `ops` 支持
  `{"afterMs":N,"op":"append","child":{...}}` 与
  `{"afterMs":N,"op":"remove","index":I}`。native 在延迟任务里构建新节点、
  用 `YGConnectorUpdateChildren` 替换 children（固定长度 List，避免
  `appendChild` 对 growable list 的依赖），随后对所有子节点调用
  `MarkYGNodeNeedsLayout` 使缓冲区尺寸变化生效。测试：红节点 → append 绿节点
  （上下各半）→ remove 红节点（绿铺满）。
- `native/engine/app_compat/apply.py`：给 `YGConnector.updateChildren`/
  `removeChild` 的 `coordinateStackingChildren()` 加
  `enableStackingPaint` 守卫；非 stacking connector 不再触发断言。重建源树后由
  `build-skyline-kernel.sh` 自动应用。
- appservice/V8 节点操作流：`tools/record-skyline-ops.js` 在 Node V8 中运行官方
  渲染层 bundle (`assets/app.js`)，抓取其发往 `SkylineGlobal` 原生绑定的节点
  操作；`SKYLINE_APP_SERVICE_OPS=@ops.json` 让嵌入式运行时回放该操作流
  （节点表 + CSS 级联子集 + flexbox 子集布局），再调用真实
  `NewYGNode`/`NewDecoratedBox`/`NewText`/`CreatePageRoute` 物化。
  `npm run test:ops` 覆盖条纹操作夹具与官方 bundle 现场录制页，并锁定确定性帧
  SHA-256；`--tap-option N` 抓取真实 `tap` -> `setData` -> `updates` 操作批次。
  回放时结构变更走 `YGConnectorUpdateChildren`，样式/文本变更走
  `DecoratedBoxUpdate*`/`TextSpanUpdate*` 细粒度回调；`@font-face` 经
  `LoadFontFace` -> `LoadResourceAsync` -> `NotifyResourceLoad` 完成字体资源
  加载；`scrollview` 节点经 `NewScrollView` 渲染，`setScrollTop` 触发
  `ScrollViewUpdateScrollTop`；宿主 `--pointer-script` 可注入触摸事件到
  Flutter 命中测试。详见 [appservice-ops.md](appservice-ops.md)。
- 运行时样式更新：opacity 节点支持
  `updateOpacity: {value, afterMs}`（`OpacityUpdateOpacity`）；view 支持
  `updateBorder: {width, color, afterMs}`（`DecoratedBoxUpdateBorder`）。native
  用 persistent handle + UI task runner 延迟调用，测试验证 1.0→0.5 透明度与
  8px 黄色边框。

## 4. 重编译流水线

```bash
# 1. 公开工具链（Dart SDK + frontend_server），约 250 MB
npm run toolchain:fetch

# 2. 固定 revision 的引擎 checkout 与基线构建（20–30 GB）
npm run engine:fetch
npm run engine:build

# 3. 安装 dart:ui 兼容层，重建 platform_strong.dill 并暂存
npm run ui:patch

# 4. 从官方 kernel 重建 package 源码树（写入 native/engine/work/，不提交）
node tools/reconstruct-kernel-sources.js --out native/engine/work/skyline-app

# 5. 用公开 SDK + 兼容层重编译 package:skyline + appservice kernel
npm run kernel:skyline

# 6. 重建引擎核心快照（使 Application 等类存在于 isolate snapshot）
npm run engine:build
npm run native:build

# 7. 可选：把运行时嵌入引擎（完整 Dart API）并渲染合成 Skyline 页面
npm run runtime:embed
npm run test:page
```

产物：

```
native/engine/work/skyline-app/packages/            1951 个源文件（不提交）
native/engine/work/skyline-app/build/flutter_assets/kernel_blob.bin
native/engine/out/linux-x64-debug/patched_sdk/      platform_strong.dill + vm_outline
```

2026-11-19 实测结果：

- kernel：80,855,168 bytes，SDK hash `a29e08c72e`（公开）；
- 212 个 `package:skyline` 源文件、73 个 FFI lookup、375 个回调、591 个
  typedef、21 个 struct，与官方清单完全一致；
- 引擎重建后，公开引擎加载重编译 kernel **不再报 `Invalid SDK hash`**，直接执行
  到 `package:sample/devtools.dart main`（缺少入口参数时抛 `RangeError`，说明
  kernel 与 `dart:ui` 已正常加载）；
- `npm run test:ui-compat`：在同一引擎上运行
  `apps/dart-ui-compat-smoke`，`Application.put/find/get`、`WindowStatus`、
  `FramePhase.vsyncTarget`、`createNativeParagraph`、`warmUpFont`、
  `SceneBuilder`/`PlatformDispatcher` 扩展全部可用，输出一帧纯绿 64×48 RGBA。

## 5. 已知限制

1. **Skyline 运行时仍是简化实现**：73 个导出已可加载，引导路径、合成页面与
   appservice/V8 节点操作流回放已跑通，attach 后增量更新、tap 手势、拖拽/滚轮
   滚动、list/grid、builder 增量 mount 与二次变更、图片原生绘制与
   `objectFit`/网络图片/`repeat`/`background-image`/SVG、资源加载、路由栈回放、
   键盘通道与 HTTP/输入通道也已覆盖，并支持
   `position: absolute|fixed`、`transform` 3D/skew/perspective、CSS `transition`
   与 `addAnimatedStyle` 时间线、输入法选区/光标/组合输入；剩余边界见
   [appservice-ops.md](appservice-ops.md) 第 6 节。
2. 私有文本标志已按官方位布局编码并接入 native（见 §2.1）。`lineHeight` 优先
   使用绝对 px 值（有字号时换算为 height 倍数），否则回退到 span `height`；
   `backgroundUseLineMetrics` 在 SkParagraph 中用完整行框绘制 span 背景与选区；
   `useCoreTextWordBreak` 已传入 SkParagraph，但只有 CoreText 分词平台会产生
   差异，Linux/Windows 与官方 Windows 一样继续使用 ICU。`ignoreClusterLetterSpacing`
   同样只作为 SkParagraph 样式标志保留，公开 line breaker 未改变簇间距行为。
3. `Canvas.clipRRect(clipOp: difference)` 已用 `Path.combine` 差值路径 +
   `clipPath` 模拟（见 `native/engine/patches/dart-ui-compat.patch`）；
   `intersect` 仍走原生快速路径。
4. 多 `Application`（每个 Shell 一个）未实现；公开引擎是单应用模型，兼容层复用
   单例 `PlatformDispatcher`。
5. 每次修改兼容层后必须 `npm run ui:patch && npm run engine:build`，否则 kernel
   引用的类不在引擎核心快照中。

## 6. 复现与校验

```bash
npm run test:kernel        # 重建源树/兼容层静态检查 + 编译级 smoke
npm run test:ui-compat     # 运行时 smoke（绿帧、Application、私有文本标志像素）
npm run test:appservice    # stub skyline 运行时 + appservice 第一帧
npm run test:page          # 嵌入式运行时 + 第一张 Skyline 页面
npm run ops:record         # 录制官方渲染层 bundle 的节点操作流
npm run test:ops           # 回放 appservice/V8 节点操作流
npm run kernel:skyline:check
npm run ui:patch:check
```

`test/kernel-rebuild.test.js` 在缺少官方 bundle、引擎 checkout 或工具链时自动
skip；设置 `SKYLINE_REQUIRE_ENGINE=1` 可让运行时 smoke 在 CI 中变为硬性失败。

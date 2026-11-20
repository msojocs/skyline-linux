# Appservice / V8 节点操作流回放

> 2026-11-19 落地。用官方渲染层 JS bundle 的真实节点操作流替换
> `SKYLINE_STUB_PAGE` 的合成 JSON widget 描述符。本文记录抓取方式、操作协议、
> 回放实现与当前边界。

## 1. 为什么不是直接嵌入 V8

官方 `skyline.node` 内的节点操作由两部分组成：

1. `skyline-addon/bundle/flutter_assets/assets/app.js`
   （exparser + skyline-js，运行时 ≈700 KB）在嵌入式 V8 中执行；
2. 该 bundle 通过 `SkylineGlobal` 原生绑定把节点操作交给 `skyline.node` 的 C++，
   C++ 再调用 `package:skyline` 注册的 Dart 回调（`NewYGNode`、
   `NewDecoratedBox`、`NewYGConnector`、`CreatePageRoute`…）。

第二阶段复刻的是第 2 部分。第 1 部分不需要重新实现：同一个 bundle 可以直接在
Node 的 V8 中运行（`vm` 模块），只要提供等价的 capture shim。这样得到的是
**官方渲染层真实产生的操作序列**，而不是手写的最终 widget 树。

```
app.js (exparser + skyline-js)
   │  PageContext.createElement / node.setAttribute / appendChild / ...
   ▼
tools/record-skyline-ops.js  (Node vm + SkylineGlobal capture shim)
   │  JSON: {"ops":[...]}
   ▼
SKYLINE_APP_SERVICE_OPS=@ops.json
   │
native/runtime (embedded)
   │  节点表 + CSS 级联 + flexbox 子集布局
   ▼
NewYGNode / NewDecoratedBox / NewText / CreatePageRoute  (真实 package:skyline)
```

## 2. 录制

```bash
npm run ops:record -- --out tmp/appservice-ops.json

# 录制一次真实 tap（触发 setData -> 节点操作更新）
npm run ops:record -- --tap-option 1 --out tmp/appservice-tap.json

# 录制路由动画：采样 JS worklet 的 addAnimatedStyle 时间线
npm run ops:record -- --tap-text "Jump To PageB" --animate-ms 300 \
  --out tmp/appservice-route-animated.json

# 或指定 bundle / 输出到 stdout
node tools/record-skyline-ops.js \
  --bundle ../wechat-web-devtools-linux/cache/devtools/resources/app.asar.unpacked/\
node_modules/skyline-addon/bundle/flutter_assets/assets/app.js
```

默认搜索路径：

```
$SKYLINE_SKYLINE_BUNDLE
../wechat-web-devtools-linux/cache/devtools/resources/app.asar.unpacked/\
node_modules/skyline-addon/bundle/flutter_assets/assets/app.js
../wechat-web-devtools-linux/tmp/official-win-resources/resources/.../assets/app.js
```

bundle 不进入本仓库；`--out` 产物只有操作日志（结构，不含 JS 源码）。录制是
确定性的：同一 bundle 每次产生相同的 `viewTag` 序列。

当前入口是 bundle 自带的 example SPA。`app_0.js`（较旧变体）会渲染 page A 的
`page-a/page-b/page-c` 路由；`app.js`（较新）渲染 animation-ball 示例。二者都可
回放，测试默认用 `app.js`。

`--tap-option <n>` 在页面 attach 后向渲染层注入一次 `tap` 事件：录制器在 exparser
树中按 `dataset.option` 找到目标，调用 `SkylineGlobal.Runtime.registerTouchCallback`
注册的回调。官方 native 层通常负责 hit-test 并合成 `tap`；capture shim 直接给出
目标元素，因此只依赖 exparser 的元素树，不依赖布局坐标。`--tap-text <text>`
按元素文本查找目标（用于 `app_0.js` 的 SPA：`--tap-text "Jump To PageB"` 会触发
`notifyNavigateTo` + 页 B 的完整操作流）。较旧的 bundle 使用
`registerEventCallback` 而不是 `registerTouchCallback`，录制器两者都接受。
tap 触发的 `setData` 会产生 attach 后的操作，录制器把它们写入 `updates`：

```json
{
  "ops": [ ... 初始页面 ... ],
  "updates": [
    { "afterMs": 300, "ops": [
        { "op": "setStyle", "id": 9, "value": "background-color: yellow;" },
        { "op": "addClass", "id": 15, "prefix": "change", "name": 1 }
    ] }
  ]
}
```

## 3. 操作协议

| 操作 | 字段 | 回放语义 |
| --- | --- | --- |
| `createElement` | `id, tag, name` | 建节点；`tag` 为 `empty`/`text`/`fragment` |
| `setText` | `id, text` | 设置文本叶内容 |
| `setAttribute` / `setAttributeValue` | `id, name, value` | `class`/`style` 走 CSS；其余存属性 |
| `setAttributes` | `id, attrs` | 批量属性（组件绑定使用） |
| `removeAttribute` | `id, name` | 删除属性 |
| `setStyle` | `id, value` | 追加内联样式 |
| `addClass` / `removeClass` | `id, prefix, name` | `prefix` 是类名，`name` 是 style scope |
| `setClass` | `id, names` | 空格分隔类名 |
| `setId` | `id, value` | 设置 `id` |
| `appendChild` / `insertBefore` / `removeChild` | `id, child[, before]` | 子树变更 |
| `replaceChild` | `id, child, index` | 位置替换 |
| `splice` | `id, pos, count, items` | exparser 的 fragment 展开插入 |
| `release` | `id` | 从父树移除并标记释放 |
| `appendStyleSheet` | `index, css` | 追加样式表（全局级联） |
| `attach` | `root[, host]` | 物化并推入一条页面路由；`attach` 之间的节点操作属于该页面 |
| `startRender` | — | 触发一次完整物化 |
| `notifyNavigateTo` | `args: [window, pageId]` | 记录下一次 `attach` 的 openType（navigateTo=4） |
| `notifySwitchTab` | `args: [window, pageId]` | 已有页面时调用 `SwitchTab`；否则下一次 attach 按 tab 页创建 |
| `notifyNavigateBack` | `args: [window, pageId, delta, animated]` | 调用 `NavigateBack`，从路由栈弹出 |
| `focusInput` | `id[, holdKeyboard, cursor, selectionStart, selectionEnd]` | 设置合成输入焦点与光标/选区；创建真实 `NewInput` 句柄并同步 `Input*` 回调 |
| `setSelection` | `id, start, end[, cursor]` | 更新选区/光标并同步输入句柄 |
| `compositionStart/Update/End` | `id, text[, start, end]` | IME 组合预览/提交（替换选区） |
| `keyboardStatus` | `inputKey, height` | 调用 `UpdateKeyboardStatus`（native → Dart 键盘高度通道） |
| `dispatchKey` | `text` | 在光标处插入文本（替换选区；测试用） |
| `addAnimatedStyle` | `id, timeline: [{atMs, style}]` 或 `id, updates` | 按时间线应用动画样式覆盖 |

`setStyleScope` / `associateComponent` / `setListenerOption` /
`setHitTestBehavior` / `createContext` / `setAsTab` / `register*Callback` /
`startMapper` 等在静态页面阶段只计数，不改变布局；`addAnimatedStyle` 已回放
`timeline`/`updates` 样式帧（JS worklet 的动态采样仍需录制器配合）。

## 3.1 增量更新

`updates` 中的批次在 `startRender` 之后按 `afterMs` 投递到 UI task runner，
按批次内容走两条路径：

1. **结构变更**（`createElement`/`appendChild`/`insertBefore`/`removeChild`/
   `replaceChild`/`splice`/`release`）：重建 attach 根的子节点，经
   `YGConnectorUpdateChildren` 交换，再 `MarkYGNodeNeedsLayout`。仅发生在
   list/grid builder 内部的批次改为 builder 同步（元数据 update、
   `ItemUpdateChildren`/`UpdateChildren`/`InsertBefore`/`RemoveChild`），不重建根。
2. **仅样式/文本变更**：逐节点 diff 计算样式，调用真实的细粒度回调：
   - `DecoratedBoxUpdateColor` / `DecoratedBoxUpdateBorder` /
     `DecoratedBoxUpdateShadow`
   - `OpacityUpdateOpacity`
   - `TextSpanUpdateText` / `TextColor` / `FontSize` / `FontWeight` /
     `FontStyle` / `LineHeight` / `LetterSpacing`
   - `TextUpdateTextAlign` / `TextUpdateMaxLines`
   - 最后 `TextApplyUpdate(text, 1)`
   然后重新测量/布局，并对所有 YGNode 调用 `MarkYGNodeNeedsLayout`。

布局结果不再通过缓存的 typed-data 指针写入：Dart GC 可能移动 typed data，
因此每个节点在**自己的 acquire/write/release 窗口**内写入 9-float buffer。
若样式变化需要新增/移除视觉 widget（背景、边框、opacity 从 1 变为非 1、
圆角变化等），该批次回退到结构重建路径。

root 自身样式也走 targeted update（背景色/圆角/gradient 等已在
`appservice ops: radius and root style updates stay targeted` 覆盖）。测试：

- `appservice ops: post-attach updates rebuild through the runtime`：内联样式覆盖、
  追加节点、删除节点（targeted + rebuild 两条路径）。
- `appservice ops: targeted text updates use TextSpanUpdate* callbacks`：
  `Hello`（红色 20px）→ `Updated text`（蓝色 30px），覆盖文本内容/颜色/字号更新。

真实手势链路由 `--tap-option` 覆盖：tap -> exparser 事件 -> `setData` ->
节点操作 -> 录制为 `updates` -> 回放 -> Flutter 重绘。
`appservice ops: a captured tap gesture drives a real update batch` 断言第一个
action-item 变为黄色。

## 3.2 资源加载平台通道

CSS `@font-face` 会触发真实的 `package:skyline` 字体资源链路：

```
@font-face { font-family: TestFont; src: url(fake-font.bin); }
  -> appservice font face
  -> LoadFontFace 回调 (Dart: font_face.dart)
  -> fetchFont -> ResourceLoader.loadResourceAsync
  -> LoadResourceAsync (native)
  -> 读取文件 -> NotifyResourceLoad 回调 (UI task runner)
  -> Dart Completer -> Flutter FontLoader
  -> NotifyFontFaceLoad (native, 状态回报)
```

路径解析：

- 绝对路径 / `file://` 原样使用；
- 相对路径相对 `SKYLINE_RESOURCE_ROOT` 解析；
- 拒绝包含 `..` 的路径。

`LoadResourceSync(runtimePtr, path)` 同步返回 `Uint8List`（缺失时为空列表）；
`LoadResourceAsync(runtimePtr, windowPtr, path, callbackId)` 在 UI task runner 上
读取文件并调用 `NotifyResourceLoad(callbackId, data)`。不能在同一 FFI 调用里
同步回调 Dart，否则会触发 VM 的 callback 限制。

测试 `appservice ops: @font-face drives the resource-loading channel` 使用临时
目录中的假字体文件，断言：

```
appservice font face: TestFont -> fake-font.bin
LoadResourceAsync: fake-font.bin -> .../fake-font.bin (ok, 15 bytes, id=1, notified=yes)
NotifyFontFaceLoad: id=1 status=2      # 假字节 -> FontFaceLoadStatus.error
```

图片资源不再依赖占位实现：`name` 为 `image`/`wx-image`/`cover-image` 的节点
由 3.5 节的 `NewDirectRenderWidget` 原生绘制。

## 3.3 scroll-view 与宿主输入

Op 流中 `name == "scrollview"` 的节点会调用真实 `NewScrollView`：

- 无 list/grid 子节点时组件类型为 `ScrollViewType.none`；含 list/grid sliver 时
  切换为 `ScrollViewType.custom`，普通子节点由 `SkylineScrollView` 自动包成
  `SliverToBoxAdapter`；
- `scrollX`/`scrollY`/`scrollEnabled` 从 `setAttributes` 读取；
- 子节点作为 Flutter `ListView` 的 items 传入，因此每个子节点必须以
  `isRoot=true` 物化（不能把 `YGNodeParentDataWidget` 直接放在非 Yoga 父节点下，
  否则会触发 `Incorrect use of ParentDataWidget`）；
- 传入一个 `kVerticalDrag` 的 `NativeGestureRecognizer` 描述符，并补齐
  `computeContentSize`/scroll 回调的 no-op C 函数（Dart 侧会无条件
  `asFunction()`）。

程序化滚动：

```json
{ "op": "setScrollTop", "id": 3, "value": 80 }
```

`setScrollTop` 调用 `ScrollViewUpdateScrollTop`，同时把 `scrollTop` 写入节点属性并
触发结构重建；当前重建会让 scroll view 以新的 `initialScrollOffset` 重新创建
（原生细粒度 scroll 更新仍待完善）。

宿主输入通道：

```bash
skyline_host ... --pointer-script script.txt
# 1200 add 160 100 1
# 1300 down 160 100 1
# 1400 move 160 60 1
# 1500 up 160 60 1
```

宿主在指定时间调用 `FlutterEngineSendPointerEvent`（touch device）。阶段名加 `m`
前缀可发送鼠标设备事件（`madd`/`mdown`/`mmove`/`mup`），并带上主键按压位。

拖拽/滚轮滚动已跑通，前提是 YGNode 的命中测试能进入子树：

- `NewYGNode` 的 `concreteWidgetType` 对带 YGConnector 子节点的容器必须传
  `kYGConnector`（3），同时把 `yogaConnector` 句柄传给 `NewYGNode`。否则
  `YGNodeRenderObject.hitTestChildren` 会走 `child.hitTest`，而
  `YGConnectorRenderObject.hitTest` 只返回 `size.contains()`，导致整棵子树的
  RenderPointerListener 都不在命中路径上（手势/滚轮事件到不了 Scrollable）。
- 修正后 `ScrollableState.setCanDrag` 收到
  `SkylineBouncingScrollPhysics -> AlwaysScrollableScrollPhysics` 的
  `shouldAcceptUserOffset=true`，`SkylineVerticalDragGestureRecognizer` 进入竞技场，
  `ShouldGestureResponse`/`ShouldAcceptGesture` 返回 1，拖拽经
  `ScrollNoopScroll` 驱动滚动。
- `PointerScrollEvent` 同样因为 `shouldAcceptUserOffset` 为真而被接受，
  `scroll: top=80 delta=(0,80)`。

鼠标拖拽同样有效（`SkylineScrollBehavior.dragDevices` 包含 `mouse`）。
键盘通道目前仍没有宿主输入路径（headless host 不产生 key 事件）。

脚本还支持鼠标滚轮信号：

```
1200 scroll 160 60 0 80
```

宿主会发送 `kFlutterPointerSignalKindScroll` 的 `PointerScrollEvent`。

## 3.4 list-view / grid-view

`name == "listview"` / `"gridview"`（也接受 `list-view`/`grid-view`）的节点会调用
真实的 `NewListView` / `NewGridView`：

- 节点物化为 sliver；位于 `scrollview` 内时直接作为 `CustomScrollView` 的 sliver
  子节点，独立出现时由运行时补一层 `ScrollViewType.custom` 的 scroll view；
- `listbuilder`/`gridbuilder` 容器被展开，`*builderitem` 视为真实 item；
- 每个 item 以 `isRoot=true` 物化，并在布局前以自身 origin 重新测量/布局
  （Flutter sliver 自己负责定位，页面级的绝对 y 不能带进 item）；
- grid 读取 `type`（0 masonry / 1 aligned）、`crossAxisCount`、
  `mainAxisSpacing`、`crossAxisSpacing`、`maxCrossAxisExtent`；aligned 模式按列宽
  `(viewport - spacing*(n-1))/n` 重新测量 item；
- list/grid 节点仍参与本地 flexbox 布局以获得视口宽高，但自身不再被 YGNode 包裹。

**builder 增量 mount**：`listbuilder`/`gridbuilder` 节点调用真实
`NewListBuilder`/`NewGridBuilder`，并把 `onBuildChild`/`onDisposeChild` 原生回调
交给 Dart sliver。Flutter 请求某个 index 时，回调把
`(builderAddress, index)` 投递到 UI task runner，再由 captured 节点表中的
`listbuilderitem`/`gridbuilderitem`（显式 `index` 属性，否则按出现顺序）
**按需**物化：

```
onBuildChild(index)
  -> NewYGNode / NewText / NewDecoratedBox ...（只构建该 item）
  -> NewListBuilderItem(children, index)
  -> ListBuilderAppendChild(builder, item)
onDisposeChild(index) -> ListBuilderRemoveChild(builder, item)
```

builder 自身不参与 YGNode 布局（sliver 由 Flutter 定位），`childCount` 可达
上千而只物化可视窗口。scrollview 的子 sliver 判定包含 builder；builder 节点
仍可带 padding/背景（传给 `NewListBuilder`/`NewGridBuilder`）。

测试夹具 `appservice-listview.ops.json`（4×80px item + 滚轮滚动）与
`appservice-gridview.ops.json`（2 列 aligned，红/绿/蓝/黄四格）；
`appservice-builder.ops.json` 声明 100 个 child、只捕获 20 个 item，断言初始
仅 mount 5 个可视项。

## 3.5 图片原生绘制（NewDirectRenderWidget）

`name` 为 `image`/`wx-image`/`img`/`cover-image` 的节点：

1. 从 `src`/`url`/`imageUrl`/`image`/`path` 属性取路径，按
   `SKYLINE_RESOURCE_ROOT` 解析并读取字节；
2. 用 Skia `SkImages::DeferredFromEncodedData` 解码（PNG/JPEG/BMP/GIF 等
   `SkCodec` 支持的格式）；
3. 用 `DlImageGPU` 包装（Dart `ui.Image` 要求 UI-thread-safe 的 `DlImage`；
   软件渲染下 unref queue 为 null 也可以）；
4. 调用 `NewDirectRenderWidget`，注册原生 paint 回调；回调把 Dart `Canvas`
   解包为引擎的 `flutter::Canvas`，通过 `DisplayListBuilder::DrawImageRect`
   直接写入当前 display list（无需构造 Dart `Image`/`Paint` 对象）。

图片拉伸模式由 `mode`/`objectFit`/`object-fit` 属性或 CSS `object-fit` 决定：

- `scaleToFill`/`fill`（默认）：铺满节点；
- `aspectFit`/`contain`：等比缩放并居中，留白；
- `aspectFill`/`cover`：等比铺满并居中，超出节点部分被裁剪；
- `center`/`none`：按原尺寸居中；`scale-down`：不超过原始尺寸的 contain；
- `widthFix`：布局阶段读取解码后的宽高比，把高度改成 `width * h / w`。

网络图片（`http(s)://`）走 Dart 的 `RequestHttpResource`（`dart:io`
`HttpClient`）：`LoadImageForNode` 先安装 direct-render widget 并投递一次带
`requestContextId` 的请求，响应在 UI task runner 上解码为 `DlImage` 后重建页面，
paint 回调即可绘制。请求期间节点保留 `image_pending`，失败则标记
`image_failed`，不会重复发起。

测试用 8×8 BMP 四色象限：拉伸到 320×240、`aspectFit` 到 200×100（左右留白）、
以及本地 HTTP server 提供的网络图片。

## 3.6 HTTP / 路由 / 输入通道

- **HTTP**：`ForwardHttpGet` 把请求转发给 Dart 的 `RequestHttpResource`
  （DevTools 下由 `dart:io HttpClient` 真正发起请求），Dart 完成后经原生 C
  回调回到 UI task runner，再调用 `OnForwardHttpGetResponse`，
  `HttpForwarder.httpGet` 的 Future 完成。`SKYLINE_HTTP_TEST_URL` 会让
  `sample/devtools.dart` 启动时发起一次 `httpGet` 并打印
  `SKYLINE_HTTP_RESULT <len> <body>`。
- **路由栈**：每个 `attach` 都物化为一条 `CreatePageRoute`。`notifyNavigateTo`
  记录 openType=navigateTo（4），初始 attach 为 kAppLaunch（0）；
  `notifySwitchTab` 调用 Dart 注册的 `SwitchTab`，已有页面时直接切 tab；
  `notifyNavigateBack` 调用 `NavigateBack(delta, animated, callbackId)` 弹栈。
  `HandlePageRoute`/`HandlePageRouteDone`/`NotifyUserGestureInProgressChange`
  由 Dart 侧回调并记录。测试夹具 `appservice-route.ops.json` 先推入红色页 B、
  再 navigateBack 回到蓝色页 A；官方 bundle 的 SPA 录制
  （`--tap-text "Jump To PageB"`）得到真实的两页操作流。
- **输入**：Dart 把指针序列经 `HandleTouchEvent`（`NativeTouchEvent` 批次）报告
  给原生，日志包含命中节点、`touchstart/touchmove/touchend` 与
  `client` 坐标；`HandleMouseEvent` 同样可解析 `NativeMouseEvent`。
- **日志**：`LogVerbose/Debug/Info/Warning/Error` 已实现（`LogTwo` 输出 tag 与
  message）。
- **键盘**：宿主 `--key-script`（`<ms> <down|up|repeat> <physical> <logical>
  [char]`）同时走两条路径：
  1. `FlutterEngineSendKeyEvent` 交给 Flutter framework；
  2. `FlutterEngineSendPlatformMessage` 在 `skyline/keyboard` 通道发送
     `"<kind> <logical> <text>"`。重编译的 appservice kernel
     （`app_compat/apply.py` 注入）在 UI 线程接收消息并调用嵌入运行时的
     `DispatchSkylineKeyEvent`，后者向焦点节点追加/删除文本并走
     `TextSpanUpdate*` 细粒度刷新。
  `focusInput` 设置焦点，`keyboardStatus` 调用 Dart 注册的
  `UpdateKeyboardStatus(inputKey, height)` 驱动 `SkylineKeyboardScrollContainer`
  的键盘高度逻辑。

## 4. 回放实现

### 4.1 节点表

`native/runtime/skyline_runtime_stub.cc` 的 `appservice::` 命名空间维护
`id -> AppNode` 表。`fragment` 在 `appendChild`/`splice` 时展开为透明容器；
`release` 后不再参与测量与物化。

### 4.2 CSS 级联子集

支持的属性：

- flex：`display`（仅 `none`）、`flex-direction`、`justify-content`、
  `align-items`、`flex`、`gap`、`flex-wrap`（忽略）
- 定位：`position: absolute|fixed`、`top/right/bottom/left`（px/%）、`z-index`
- 盒模型：`width/height`（px/%/auto）、`min/max-*`、`padding`/`margin` 及四边，
  `border`/`border-width`/`border-color`、`border-radius`
- 绘制：`background`/`background-color`、`opacity`、`object-fit`、`transform`
  （`translate`/`scale`/`rotate`/`matrix`，绕元素中心）、`box-shadow`
  （首层 offset/blur/spread/color/inset）
- 文本：`font-size`、`font-weight`、`font-style`、`line-height`（px 或倍数）、
  `letter-spacing`、`text-align`、`-webkit-line-clamp`

选择器支持元素名、`.class`、`#id`、组合（如 `wx-button.plain`）与逗号列表；
按 specificity + 源顺序级联，内联 `style` 最后覆盖。字号/颜色/行高/字重等文本
属性按 CSS 继承规则从父节点继承。后代选择器目前取最后一个复合选择器。

### 4.3 布局

- 容器默认 `display:flex; flex-direction:column; align-items:stretch`。
- 两遍布局：先自底向上测量（文本按字号/字符宽度估算，支持换行），再自顶向下
  按 flex/justify/align/margin/padding 分配矩形。
- `position: absolute/fixed` 子节点不参与 flex 流；在父 content box 上解析
  `top/right/bottom/left`（px 或 %），同时给出 left+right（或 top+bottom）且未显式
  设置宽高时拉伸到剩余空间。`transform` 只影响绘制，不改变布局。
- 每个节点的 `top/left/width/height` 写入其 9-float yoga buffer；root 强制为
  视口尺寸（取自 `CreateWindow` 的 width/height）。
- 文本测量是近似值（ASCII 0.56em、CJK 1.0em、空格 0.3em），用于在无字体度量
  的情况下得到稳定的 flex 布局；字形本身仍由 Flutter 文本引擎绘制。

### 4.4 物化

自底向上构造真实 Skyline widget：

```
text   -> NewTextSpan + NewText
view   -> NewYGConnector(children) [+ NewDecoratedBox] [+ NewOpacity]
scroll -> NewScrollView(children)
list   -> NewListView(item widgets)        (sliver)
grid   -> NewGridView(item widgets)        (sliver)
builder-> NewListBuilder/NewGridBuilder     (sliver, on-demand items)
image  -> NewDirectRenderWidget(onPaint)   (native DrawImageRect)
任何节点 -> NewYGNode(address, Float32List(9), child[, yogaConnector])
每次 attach -> CreatePageRoute(rootYGNode)  (路由栈)
```

容器的 `concreteWidgetType` 必须与其 child 一致：带 YGConnector 的节点传
`kYGConnector` 并同时传 `yogaConnector`，scroll/list/grid/image 分别传对应的
widget 类型；否则命中测试无法进入子树。

`NewTextSpan` 的 `height` 是 Flutter 行高倍数，CSS `line-height` 会先除以
`font-size` 再传入（否则 14px 会被解释成 14 倍行高，文本被推出可视区）。

## 5. 测试

```bash
npm run test:ops
```

- `committed stripe fixture`：`test/fixtures/appservice-stripes.ops.json` 用类选择器
  与内联样式驱动三条 80px 色带，断言像素与整帧 SHA-256；不依赖官方 bundle。
- `real exparser bundle`：用 `tools/record-skyline-ops.js` 现场录制官方 bundle，
  回放后断言 `#ccc` 操作栏、白色按钮、深色文本与绿色 `rect`。bundle 不存在时
  该用例跳过。
- `post-attach updates`：`test/fixtures/appservice-update.ops.json` 按
  `afterMs` 执行内联样式覆盖、追加与删除节点，断言最终色带位置。
- `captured tap gesture`：录制 `--tap-option 1` 的真实 `setData` 更新批次，
  回放后断言被 tap 的按钮变为黄色。
- `targeted text updates`：文本内容、颜色、字号经 `TextSpanUpdate*` 更新。
- `scrollview renders clipped content`：120px scroll view 裁剪 400px 内容。
- `ScrollViewUpdateScrollTop scrolls the content`：程序化滚动到 80px。
- `host drag gesture scrolls the scroll view`：touch 拖拽经真实手势协商滚动
  （扣除 touch slop 后约 22px）。
- `mouse wheel scrolls the scroll view`：80px 滚轮位移。
- `mouse drag scrolls the scroll view`：`m*` 指针脚本（鼠标设备）拖拽滚动。
- `list-view renders a virtualized sliver`：4×80px item 的 sliver 裁剪 + 滚轮滚动。
- `grid-view lays out a 2-column aligned grid`：2 列 aligned 四色网格。
- `image nodes paint through NewDirectRenderWidget`：8×8 BMP 四色象限拉伸绘制。
- `ForwardHttpGet bridges to RequestHttpResource`：本地 HTTP server ->
  `ForwardHttpGet` -> Dart `HttpClient` -> `OnForwardHttpGetResponse` ->
  `SKYLINE_HTTP_RESULT`。
- `input and route channels receive Dart events`：`HandleTouchEvent`
  （touchstart/move/end + client 坐标）与 `HandlePageRouteDone`。
- `host pointer script reaches the Skyline hit test path`：`--pointer-script`
  的 embedder 指针事件命中 YGNode 树。
- `font-face resource channel`：`@font-face` -> `LoadFontFace` ->
  `LoadResourceAsync` -> `NotifyResourceLoad` -> `NotifyFontFaceLoad`。
- `list-builder incremental mount`：100 个声明 child 只捕获 20 个 item，初始
  仅 mount 可视的 5 个（`NewListBuilder` + `ListBuilderAppendChild`）。
- `route stack push/pop`：`notifyNavigateTo` + attach 推入页 B，
  `notifyNavigateBack` 弹回页 A，断言两次 `CreatePageRoute` 与最终像素。
- `recorded SPA navigation`：官方 `app_0.js` 的 "Jump To PageB" tap 录制出真实
  两页操作流，回放后 `didPush pageId: 1` 且页 B 文本可见。
- `image objectFit`：`aspectFit` 将 8×8 位图缩放到 100×100 并在 200×100 的
  节点中居中留白。
- `network image`：本地 HTTP server -> `RequestHttpResource` -> 解码 -> 重建
  页面 -> 四象限像素。
- `host keyboard channel`：`--key-script` 触发 `FlutterEngineSendKeyEvent` 与
  `skyline/keyboard` 平台消息，运行时把 `A` 更新为 `Ab`；同时验证
  `UpdateKeyboardStatus`。
- `absolute layout`：flow 子节点不受影响，right/top、left/bottom 与
  left+right 拉伸三种绝对定位。
- `CSS transforms`：`translate` 只移动绘制、`scale(0.5)` 绕中心缩放，布局槽位
  保持不变。
- `box-shadow`：`10px 10px 0 rgba(0,0,0,.5)` 在节点右/下边缘绘制 50% 黑阴影。

## 6. 当前边界

- attach 后的样式/文本更新走细粒度回调；结构变更仍会重建 attach 根子树。
  背景/边框/box-shadow/圆角/gradient 与 root 自身样式均支持增量更新
  （`DecoratedBoxUpdateColor/Border/Shadow/BorderSideRadius/Gradient/
  ClearGradient`）。flexbox 子集支持 `align-self`、`order`、`row-gap`/
  `column-gap`（含 `gap: r c`）、`flex-wrap` + `align-content`，逐边
  `border-width/color`，逐角 `border-radius`（px/%），以及 `overflow:
  hidden/clip`（`NewClipRRect`，圆角增量走 `ClipRRectUpdateRadius`）。
- `transition` 已回放：CSS `transition`/`transition-*` 解析为属性掩码、时长、
  延迟与缓动，16ms 时间线通过 target/override 逐帧走
  `DecoratedBoxUpdateColor`/`OpacityUpdate`/`TextSpanUpdate*` 与
  `YGNodeSetupTransform`。`addAnimatedStyle` 支持录制/回放
  `{timeline:[{atMs,style:{...}}]}`；录制器 `--animate-ms` 会驱动 JS worklet
  mapper（含 derived value）并采样为 `updates` 时间线。
- `transform` 已覆盖 `translate/translate3d/scale/scale3d/rotate/rotateX/
  rotateY/skewX/skewY/perspective/matrix/matrix3d`；不改变布局。
  CSS `transform-origin` 支持 px/%/关键字（默认 50% 50%）。
- `position: fixed` 在页面构建时提升到根连接器，因此滚动祖先不会移动它；
  insets 按 viewport 解析。
- builder 已按需 mount，并支持 attach 后的二次变更：
  `UpdateChildCount/ChildHeight/CrossAxisCount/...`、item 内容
  `ItemUpdateChildren`、index `ItemUpdateIndex`、`UpdateChildren`、
  `InsertBefore`/`RemoveChild`；仅 builder 内部的批次不再整页重建。
- 图片支持 `fill`/`contain`/`cover`/`none`/`scale-down`、`widthFix`、
  `repeat`/`repeat-x`/`repeat-y`；CSS `background-image` 经
  `NewDecoratedBox` 的 onPaint 回调原生绘制；SVG 经 Dart 侧 `SvgToRgba`
  （flutter_svg）解码为 RGBA 后包装成 DlImage。网络/本地/背景图共用 URL 缓存，
  重复 URL 只发一次请求；节点释放或 `src` 变化时调用
  `CancelRequestHttpResource`。
- 合成键盘输入维护 UTF-8 光标与选区：字符插入替换选区，Left/Right/Home/End、
  Backspace/Delete 生效；`setSelection` 与
  `compositionStart/Update/End` 支持 IME 组合预览与提交。`name=="input"` 的节点
  额外通过 `NewInput` 创建一个离屏 `SkylineInput` 句柄，`InputValue/Cursor/
  SelectionStart/End/HoldKeyboard/Focus` 回调均在该句柄上实际调用；文本仍由
  `NewText` 渲染以保持确定性帧。
- native view：`tag=="texture"` 节点经 `NewTexture(id, type, useTexture)` 创建
  `SkylineTextureView`，按 JS `viewId` 建立索引；Linux 无平台视图注册表，使用
  黑色占位 slot + `RenderPointerListener`，`HandleNativeViewPointerEvent`
  解析 `NativePointerEvent` 并按 `viewId` 定位节点输出 down/move/up 局部坐标
  （`SKYLINE_APP_SERVICE_DEBUG`）。`TextureDispose` 在节点释放/重建时清理通道
  状态；`test:ops` 覆盖指针转发与释放。
- `LoadResourceSync`/`LoadResourceAsync` 共用按解析后路径的资源缓存（≤4 MB，
  日志带 `, cache`）；`data:` 内存字体由 Dart `fetchFont` 直接解码，native
  只需完成普通文件/网络资源通道。`AddPendingFont`/`RemovePendingFont` 仍为
  no-op（字体加载状态由 Dart 侧 FontFace 管理）。
- 文本 span 通过 `fontFamilyFallback` 传入常见 CJK/系统字体；宿主无 CJK 字体时
  仍会显示 tofu。私有 `backgroundUseLineMetrics`/`useCoreTextWordBreak` 已按官方
  位布局编码并由 native 文本实现解码（CSS line-height 仍可经 span height 生效）；
  `backgroundUseLineMetrics` 在 SkParagraph 中用完整行框绘制背景/选区，
  `useCoreTextWordBreak` 只在 CoreText 分词平台产生差异，Linux/Windows 与官方
  Windows 一样继续使用 ICU。
- L5：Linux 侧确定性帧哈希与 `test/lib/l5.js` 容差比较已就绪
  （`npm run test:l5`）。官方 Windows 引擎帧由 `tools/l5-windows.js` 在
  Wine 渲染进程中以 dpr=2 采集，基线写入 `spec/l5-baseline.win32.json`；
  Windows 原始帧为 BGRA，采集工具统一转 RGBA。7 个夹具逐像素完全一致（`shadow` 最大差 1），
  `overflow`/`fixed-scroll` 仅有边缘级差异，`transform3d` 与
  `gridview`/`listview` 的已知差异见 `docs/feasibility.md` §3.1。

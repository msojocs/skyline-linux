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
目标元素，因此只依赖 exparser 的元素树，不依赖布局坐标。tap 触发的
`setData` 会产生 attach 后的操作，录制器把它们写入 `updates`：

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
| `attach` | `root[, host]` | 记录页面根 |
| `startRender` | — | 触发一次完整物化 |

`setStyleScope` / `associateComponent` / `setListenerOption` /
`setHitTestBehavior` / `addAnimatedStyle` / `createContext` / `setAsTab` /
`register*Callback` / `startMapper` 等在静态页面阶段只计数，不改变布局。

## 3.1 增量更新

`updates` 中的批次在 `startRender` 之后按 `afterMs` 投递到 UI task runner，
按批次内容走两条路径：

1. **结构变更**（`createElement`/`appendChild`/`insertBefore`/`removeChild`/
   `replaceChild`/`splice`/`release`）：重建 attach 根的子节点，经
   `YGConnectorUpdateChildren` 交换，再 `MarkYGNodeNeedsLayout`。
2. **仅样式/文本变更**：逐节点 diff 计算样式，调用真实的细粒度回调：
   - `DecoratedBoxUpdateColor` / `DecoratedBoxUpdateBorder`
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

root 自身样式在 attach 后仍不更新。测试：

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

测试夹具 `appservice-listview.ops.json`（4×80px item + 滚轮滚动）与
`appservice-gridview.ops.json`（2 列 aligned，红/绿/蓝/黄四格）。

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

图片拉伸到节点宽高（等价 `object-fit: fill`）；`objectFit`/`repeat` 等属性尚未
接入。测试用 8×8 BMP 四色象限拉伸到 320×240，断言四个象限颜色。

## 3.6 HTTP / 路由 / 输入通道

- **HTTP**：`ForwardHttpGet` 把请求转发给 Dart 的 `RequestHttpResource`
  （DevTools 下由 `dart:io HttpClient` 真正发起请求），Dart 完成后经原生 C
  回调回到 UI task runner，再调用 `OnForwardHttpGetResponse`，
  `HttpForwarder.httpGet` 的 Future 完成。`SKYLINE_HTTP_TEST_URL` 会让
  `sample/devtools.dart` 启动时发起一次 `httpGet` 并打印
  `SKYLINE_HTTP_RESULT <len> <body>`。
- **路由**：`HandlePageRoute` / `HandlePageRouteDone` /
  `NotifyUserGestureInProgressChange` 解析 Dart 参数并在
  `SKYLINE_APP_SERVICE_DEBUG` 下打印；初始页面会观察到
  `HandlePageRouteDone: pageId=0`。
- **输入**：Dart 把指针序列经 `HandleTouchEvent`（`NativeTouchEvent` 批次）报告
  给原生，日志包含命中节点、`touchstart/touchmove/touchend` 与
  `client` 坐标；`HandleMouseEvent` 同样可解析 `NativeMouseEvent`。
- **日志**：`LogVerbose/Debug/Info/Warning/Error` 已实现（`LogTwo` 输出 tag 与
  message）。
- **键盘**：`InputHoldKeyboard`/`UpdateKeyboardStatus` 是 Dart 注册回调；headless
  host 目前没有 key 事件输入路径，通道保留待接入。

## 4. 回放实现

### 4.1 节点表

`native/runtime/skyline_runtime_stub.cc` 的 `appservice::` 命名空间维护
`id -> AppNode` 表。`fragment` 在 `appendChild`/`splice` 时展开为透明容器；
`release` 后不再参与测量与物化。

### 4.2 CSS 级联子集

支持的属性：

- flex：`display`（仅 `none`）、`flex-direction`、`justify-content`、
  `align-items`、`flex`、`gap`、`flex-wrap`（忽略）
- 盒模型：`width/height`（px/%/auto）、`min/max-*`、`padding`/`margin` 及四边，
  `border`/`border-width`/`border-color`、`border-radius`
- 绘制：`background`/`background-color`、`opacity`
- 文本：`font-size`、`font-weight`、`font-style`、`line-height`（px 或倍数）、
  `letter-spacing`、`text-align`、`-webkit-line-clamp`

选择器支持元素名、`.class`、`#id`、组合（如 `wx-button.plain`）与逗号列表；
按 specificity + 源顺序级联，内联 `style` 最后覆盖。字号/颜色/行高/字重等文本
属性按 CSS 继承规则从父节点继承。后代选择器目前取最后一个复合选择器。

### 4.3 布局

- 容器默认 `display:flex; flex-direction:column; align-items:stretch`。
- 两遍布局：先自底向上测量（文本按字号/字符宽度估算，支持换行），再自顶向下
  按 flex/justify/align/margin/padding 分配矩形。
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
image  -> NewDirectRenderWidget(onPaint)   (native DrawImageRect)
任何节点 -> NewYGNode(address, Float32List(9), child[, yogaConnector])
root   -> CreatePageRoute(rootYGNode)
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

## 6. 当前边界

- attach 后的样式/文本更新走细粒度回调；结构变更仍重建 attach 根子树。
  渐变/阴影/圆角等属性更新与 root 自身样式尚未细粒度处理。
- `position: absolute`、`transform`、`transition` 暂按普通流处理。
- list/grid 的虚拟化 item 由录制时一次性给出；`ListBuilderAppendChild` 等增量
  mount 回调没有回放，builder 容器按静态子树展开。
- 图片只支持 `object-fit: fill`（拉伸），`objectFit`/`repeat`/`background-image`
  尚未接入；网络图片（`http(s)://`）仍走文件读取路径。
- 键盘没有宿主输入路径；`HandleNativeViewPointerEvent` 等仍为 no-op。
- `LoadResourceSync` 已实现但当前字体链路只走异步接口；缓存、内存字体未接入。
- 中文字形依赖宿主字体；当前 Linux host 没有 CJK 字体时会显示 tofu 方框。
- L5 的 Windows ↔ Linux 像素对比仍受官方 appservice guest 阻塞；本阶段提供的是
  确定性输入与 Linux 侧帧哈希，为 L5 准备可比对的基线。

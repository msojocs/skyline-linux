# 候选回调/行为覆盖矩阵（D3）

来源：`npm run gaps` 列出的“事件/观察/命令”候选，加上对
`native/runtime/skyline_runtime_stub.cc` 的逐符号核对。

判定方法：

```bash
# 候选名在运行时源码中的出现次数（0 = 原生侧从未调用该注册回调）
rg -n '\bScrollViewUpdateOnScroll\b' native/runtime/skyline_runtime_stub.cc
```

`New*`/`Append*`/`Insert*`/`Update*` 属性构造类回调由 C++ 直接构建 Flutter
widget，不经过 Dart 回调表，不计为缺口。下表的“未接线”表示 C++ 中完全没有
对应用到的回调名。

## 1. 已接线（72 / 375 通过 FindCallback）

`runtimeWiring()` 输出的已接线清单是权威来源；常见的有：
`NewYGNode`、`NewDecoratedBox`、`NewText`、`NewTextSpan`、`NewScrollView`、
`NewListView`、`NewGridView`、`NewSwiper`、`NewStickyHeader/Section`、
`NewDirectRenderWidget`、`ScrollViewUpdateScrollTop`、
`ScrollViewUpdateScrollIntoView`、`InputValue/Cursor/Selection*/Focus`、
`TextSpanUpdate*`、`BackgroundImageMarkNeedPaint`、`RequestHttpResource`、
`SvgToRgba` 等。

## 2. 未接线但由其他路径覆盖（非缺口）

| 回调 | 覆盖路径 |
| --- | --- |
| `MeasureText` / `MeasureInput` | 选择器查询走 `SkylineGetNodeRect`；文本尺寸由 C++ `TextIntrinsicWidth`/`CountTextLines` 计算 |
| `GetWindowWidth` / `GetWindowHeight` | `PageContext.clientWidth/clientHeight` 在 createWindow 时捕获 |
| `HitTestNode` | appservice 镜像树由 `__skylineDebug.hit` / `touchTargetElement` 命中测试 |
| `NotifyFontFaceLoad` | 字体平台通道 / 运行时导出函数，不经过回调表 |
| `DumpElementTree` / `DumpRenderTree` | 仅有调试/自动化价值，未实现；不影响页面功能 |

## 3. 确认缺失（源码 0 引用）

| 能力 | 回调 | 影响 |
| --- | --- | --- |
| 下拉刷新 | `ScrollViewRequestRefresherRefresh` / `TwoLevel` | `refresher-enabled` 的刷新请求不回调 |
| Swiper 事件/控制 | `SwiperUpdateOnChange` / `OnScroll`、`SwiperAnimateToPage` / `JumpToPage` | `bindchange`/`bindscroll` 与 `swiper` 实例方法缺失 |
| 输入框编程式焦点 | `InputFocusManually` / `InputBlurManually` | 点击聚焦可用（`InputFocus`），`wx.hideKeyboard`/编程式 focus 不可用 |
| IntersectionObserver | `YGNodeAddIntersectionObserver` / `Remove...` | 节点可见性回调不触发 |
| Selection API | `SelectionSetRange` / `OnSelectionChange` / `SelectionColor` 等 | 文本选区 API/事件缺失 |
| PickerView 事件 | `PickerViewColumnUpdateOnChange/Start/End` | `bindchange`/`bindpickstart`/`bindpickend` 不触发 |
| 返回拦截 | `SetNavigateBackInterception` | `wx.enableAlertBeforeUnload` 类能力缺失 |
| 用户手势 | `StartUserGesture` / `StopUserGesture` | 自动播放/音频的用户手势状态不可用 |
| 无障碍 | `UpdateAccessibility`、`Semantics*`、`AccessibilityBoldTextEnabled` | 语义树不更新；`UpdateAccessibility` 是空 stub |
| 自动化/调试 | `SnapshotTakeSnapshot`、`DumpElementTree`、`CollectGarbage`、`SendMessageToRunner`、`RelaunchApp`、`ShutdownApplication` | DevTools 自动化、内存压力、runner 消息缺失 |

## 4. 已实现并加行为探针

| 特性 | 实现 | 探针 |
| --- | --- | --- |
| 滚动事件 | `NewScrollView` 传入 `ScrollStartEvent`/`ScrollEvent`/`ScrollEndEvent`，并用 `ScrollViewUpdateOnScrollStart/OnScroll/OnScrollEnd` 安装；回调更新 `scrollTop` 并经 `SkylineSetScrollEventSink` 投递到 addon JS 线程，bootstrap 分发给 `bindscroll` 回调与 `onScroll-<tag>` worklet | `test/bootstrap-scroll.test.js`：sink → bindscroll/worklet 两条路径；`test:ops` 断言 `scroll callbacks installed #<id> (start=1 scroll=1 end=1)` 与滚动日志；Linux DevTools address-book 滚动后 A-Z 高亮 A→B |
| `setData(data, callback)` | `PageContext.render(callback)` 在 flush 后调用回调（SDK 的 `triggerRender` 路径）；address-book 的 `computedSize()` 依赖该回调填充 `_tops` | `test/bootstrap-scroll.test.js`：render 回调与 op flush |
| `pointer-events: none` | 解析为 `PointerEvents.none` 传给 `NewYGNode`；`SkylineGetNodeRect` 增加 `pointerEvents` 字段，appservice 镜像 hit test 跳过该子树 | `native/test/skyline-appservice-ops.test.js`：`pointer-events:none` 覆盖层可穿透滚动；对照组 `pointer-events` auto 覆盖层拦截滚动 |
| `will-change` | 纯渲染提示，无静态像素差异；不解析（官方探针也不产生差异） | 无行为差异，不设探针 |

## 5. 后续修复任务

- ~~**D7** 滚动事件~~ 已完成：`ScrollViewUpdateOnScroll/OnScrollStart/OnScrollEnd`
  在滚动位置/状态变化时回调，并已用 address-book 的滚动高亮验证。
- **D8** Swiper/PickerView/Input 编程式 API 与事件回调。
- **D9** IntersectionObserver、Selection、Refresher、返回拦截。
- **D10** 无障碍语义树与 DevTools 自动化快照/内存 API。

新增探针时沿用 D2 的方式：静态行为夹具优先放在 `test:ops`（可断言日志/像素），
需要官方 Windows 基准时再用 `tools/l5-windows.js --only <name>` 采集。

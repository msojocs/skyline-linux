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
| 用户手势 | `StartUserGesture` / `StopUserGesture` | 自动播放/音频的用户手势状态不可用 |
| 无障碍 | `UpdateAccessibility`、`Semantics*`、`AccessibilityBoldTextEnabled` | 语义树不更新；`UpdateAccessibility` 是空 stub |
| 自动化/调试 | `SnapshotTakeSnapshot`、`DumpElementTree`、`CollectGarbage`、`SendMessageToRunner`、`RelaunchApp`、`ShutdownApplication` | DevTools 自动化、内存压力、runner 消息缺失 |

## 4. 已实现并加行为探针

| 特性 | 实现 | 探针 |
| --- | --- | --- |
| 滚动事件 | `NewScrollView` 传入 `ScrollStartEvent`/`ScrollEvent`/`ScrollEndEvent`，并用 `ScrollViewUpdateOnScrollStart/OnScroll/OnScrollEnd` 安装；回调更新 `scrollTop` 并经 `SkylineSetScrollEventSink` 投递到 addon JS 线程，bootstrap 分发给 `bindscroll` 回调与 `onScroll-<tag>` worklet | `test/bootstrap-scroll.test.js`：sink → bindscroll/worklet 两条路径；`test:ops` 断言 `scroll callbacks installed #<id> (start=1 scroll=1 end=1)` 与滚动日志；Linux DevTools address-book 滚动后 A-Z 高亮 A→B |
| `setData(data, callback)` | `PageContext.render(callback)` 在 flush 后调用回调（SDK 的 `triggerRender` 路径）；address-book 的 `computedSize()` 依赖该回调填充 `_tops` | `test/bootstrap-scroll.test.js`：render 回调与 op flush |
| Swiper | 真实 `NewSwiper` + `SwiperUpdateOnChange/OnScroll`；`current` 属性、`swiperTo` op、`native.swiperTo` 走 `SwiperAnimateToPage`/`JumpToPage`；bootstrap 分发给 `onChangeEvent`/`onScrollEvent` 与 `onChange`/`onTransition` worklet | `test:ops`：换页像素 + `swiper change/scroll` 日志；`test/bootstrap-scroll.test.js`：三条 JS 分发；DevTools `tab-indicator` 点击/`swiperTo` 均触发 `bindchange` |
| PickerView | 真实 `NewPickerViewColumn` + `PickerViewColumnUpdateOnChange/OnStart/OnEnd`；`value` 属性、`pickerTo` op、`native.pickerColumnSetCurrent` 走 `UpdateCurrent`；change 合成 picker-view `{value:[...]}` detail | `test:ops`：列 change/start/end 日志与编程式选中像素 |
| Input 编程式焦点 | `focusInput`/`blurInput`、`focus` 属性与 `native.inputFocus` 调用 `InputFocusManually`/`InputBlurManually` | `test:ops`：`InputFocusManually`/`InputBlurManually` 日志；bootstrap 单测记录 focus/blur 事件 |
| IntersectionObserver | `YGNodeAdd/RemoveIntersectionObserver` 注册到真实 Dart YGNode；导出 `YogaTriggerIntersection(Observers)` 回传 rects/ratio/isIntersecting，经 JSON 通道分发给 JS observer | `test:ops`：注册/阈值触发日志；bootstrap 单测：registration + entry callback |
| Selection | `<selection>` 创建 `NewSelection`，`SelectionUpdateOnSelectionChange` 上报 `selectedString`/包围盒；`SelectionSetRange` 用文本 span 地址设置区间 | `test:ops`：setRange/change 日志；bootstrap 单测：selection 事件 |
| Refresher | `NewScrollView` 映射 refresher 属性并接入 Pulling/WillRefresh/Refresh/Restore/Abort/Status 回调；`ScrollViewRequestRefresherRefresh/TwoLevel` 支持编程式开合 | `test:ops`：编程式下拉触发 pulling/status/refresh；bootstrap 单测：refresher 分发 |
| 返回拦截 | `SetNavigateBackInterception(pageId, enabled)` 下发 Dart 路由；`OnNavigateBackIntercept` 导出经 JSON 通道通知 JS | `test:ops`：enabled/OnNavigateBackIntercept 日志；bootstrap 单测：navigate-back 事件 |
| `pointer-events: none` | 解析为 `PointerEvents.none` 传给 `NewYGNode`；`SkylineGetNodeRect` 增加 `pointerEvents` 字段，appservice 镜像 hit test 跳过该子树 | `native/test/skyline-appservice-ops.test.js`：`pointer-events:none` 覆盖层可穿透滚动；对照组 `pointer-events` auto 覆盖层拦截滚动 |
| `will-change` | 纯渲染提示，无静态像素差异；不解析（官方探针也不产生差异） | 无行为差异，不设探针 |

## 5. 后续修复任务

- ~~**D7** 滚动事件~~ 已完成：`ScrollViewUpdateOnScroll/OnScrollStart/OnScrollEnd`
  在滚动位置/状态变化时回调，并已用 address-book 的滚动高亮验证。
- ~~**D8** Swiper/PickerView/Input~~ 已完成：真实 Swiper/PickerViewColumn
  回调与 `AnimateToPage`/`JumpToPage`/`UpdateCurrent`，Input
  `FocusManually`/`BlurManually` 均已接入。
- ~~**D9** IntersectionObserver、Selection、Refresher、返回拦截~~ 已完成：
  `YGNodeAdd/RemoveIntersectionObserver` + trigger 回传、`NewSelection` 与
  `SelectionSetRange`、refresher 原生回调与编程式开合、
  `SetNavigateBackInterception`/`OnNavigateBackIntercept` 均已接入。
- **D10** 无障碍语义树与 DevTools 自动化快照/内存 API。

新增探针时沿用 D2 的方式：静态行为夹具优先放在 `test:ops`（可断言日志/像素），
需要官方 Windows 基准时再用 `tools/l5-windows.js --only <name>` 采集。

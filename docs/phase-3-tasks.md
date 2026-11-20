# 阶段 3 任务清单

> 最后更新：2026-11-20
> 分支：`feature/phase3-gesture-list-image-channels`
> 进度格式：**x / y** = 已完成任务 / 总任务
>
> 核心验收：**22 / 22（100%）**
> 长尾/外部阻塞：**1 / 12**
> 全部任务：**23 / 34（67.6%）**
>
> 勾选项表示已在 Linux 回放运行时上实现并通过测试；标有 `blocked` 的任务
> 依赖外部环境或私有引擎能力。

## 进度总览

| 分组 | 已完成 | 总数 | 进度 |
| --- | ---: | ---: | ---: |
| A. 原始阶段 3 验收项 | 8 | 8 | 100% |
| B. 后续运行时增量 | 14 | 14 | 100% |
| C. 长尾 / 外部阻塞 | 1 | 12 | 8.3% |
| **合计** | **23** | **34** | **67.6%** |

## A. 原始阶段 3 验收项（8 / 8）

- [x] **A1** CSS `transition` 与 `addAnimatedStyle` 时间线回放
  - `transition`/`transition-*` 属性掩码、时长、延迟、缓动；16ms target/override 时间线。
  - `addAnimatedStyle` 支持 `timeline:[{atMs,style}]` 与 `updates` 样式帧。
  - 录制器 `--animate-ms` 驱动 JS worklet/derived value 采样（见 B7）。
- [x] **A2** builder 二次变更细粒度回调
  - `UpdateChildCount/ChildHeight/CrossAxisCount/...`、`ItemUpdateChildren`、
    `ItemUpdateIndex`、`UpdateChildren`、`InsertBefore`/`RemoveChild`；
    builder-only 批次不再整页重建。
- [x] **A3** 输入法选区/光标/组合输入与 `InputHoldKeyboard`
  - UTF-8 光标/选区、方向键/Home/End/Delete、`setSelection`、
    `compositionStart/Update/End`；`name=="input"` 创建真实 `NewInput` 句柄。
- [x] **A4** `position: fixed` 视口定位
  - insets 按 viewport 解析；固定节点提升到页面根连接器（见 B6）。
- [x] **A5** transform 3D/skew/perspective
  - `translate3d/scale3d/rotateX/rotateY/skewX/skewY/perspective/matrix3d`。
- [x] **A6** 图片扩展
  - `repeat/repeat-x/repeat-y`、CSS `background-image`、Dart `SvgToRgba` 解码、
    URL 缓存/等待者、`CancelRequestHttpResource`。
- [x] **A7** 文本 lineHeight / CJK
  - CSS line-height 经公开 span `height` 生效；CJK `fontFamilyFallback`。
  - 私有 `dart:ui` 文本标志仍受限，见 C2。
- [x] **A8** L5 Linux 基线/harness
  - `test/lib/l5.js`、`tools/l5-baseline.js`、`spec/l5-baseline.json`、
    `npm run test:l5`；Windows 官方 guest 采集见 C1。

## B. 后续运行时增量（14 / 14）

- [x] **B1** `transform-origin`（px / % / left/top/right/bottom/center）
- [x] **B2** 逐角 `border-radius`（1–4 值、px/%）与 `DecoratedBoxUpdateBorderSideRadius`
- [x] **B3** 逐边 border（`border-top/right/bottom/left`、1–4 值 width/color）
- [x] **B4** CSS linear-gradient 解析与 `DecoratedBoxUpdateGradient` / `ClearGradient`
- [x] **B5** root 自身样式 targeted update（背景/圆角等）
- [x] **B6** fixed 节点提升到页面根连接器（滚动祖先内保持固定）
- [x] **B7** 录制器 `--animate-ms` / `--animate-step-ms` worklet 采样
- [x] **B8** `align-self`（stretch/start/center/end/auto）
- [x] **B9** CSS `order` 稳定排序
- [x] **B10** `row-gap` / `column-gap` / `gap: r c`
- [x] **B11** `flex-wrap` + `align-content`（row/column、行内 flex 与对齐）
- [x] **B12** `overflow: hidden/clip` + `ClipRRectUpdateRadius`
- [x] **B13** 资源路径缓存（`LoadResourceSync`/`Async`，≤4 MB）
- [x] **B14** L5 夹具扩展（transform-origin/fixed-scroll/align-gap/flex-wrap/order/overflow）

## C. 长尾 / 外部阻塞（1 / 12）

- [x] **C1** 官方 Windows appservice guest 的真实 L5 采集与像素对比
  - `tools/l5-windows.js` 在 Wine + 官方 Windows Electron 中以 dpr=2 运行
    `skyline.node`，窗口就绪后经 `notifyAppLaunch` + `PageContext` 回放夹具操作，
    从共享内存读取 640x480 帧。原始 Windows 帧为 BGRA，工具统一转为 RGBA。
  - `spec/l5-baseline.win32.json` 保存 13 个夹具的 zlib 压缩帧、样本点与容差；
    `npm run test:l5` 在 Linux 以 dpr=2 重放同一夹具并逐像素比较。
  - 结果：7 个夹具逐像素完全一致，`shadow` 最大差 1；`overflow`/`fixed-scroll`
    失配率 < 0.2%（裁剪/滚动边缘）；`transform3d` 存在 3D 变换保真差异；
    `gridview`/`listview` 的合成 op 流无法触发官方 native builder 回调，标记为
    不可比。详见 `docs/feasibility.md` §3.1。
  - 采集命令：`npm run l5:windows`；复用已采集帧：`npm run l5:windows:reuse`。
- [ ] **C2** 私有 `dart:ui` 文本标志
  - `blocked`：`backgroundUseLineMetrics`、`useCoreTextWordBreak` 仅能编译，
    公开引擎文本实现不会应用；需在 `skyline_ext` 文本实现中处理。
- [ ] **C3** `HandleNativeViewPointerEvent` / native view 通道
- [ ] **C4** `flex-basis` / `flex-shrink`
- [ ] **C5** `background-position` / `object-position`
- [ ] **C6** `box-sizing`（`content-box`）
- [ ] **C7** 百分比 `padding` / `margin`
- [ ] **C8** `position: sticky`
- [ ] **C9** radial / conic gradient
- [ ] **C10** 显式 `background-size` 长度值（非 cover/contain）
- [ ] **C11** `transform-origin` 的 z 分量 / 3D origin
- [ ] **C12** `AddPendingFont` / `RemovePendingFont` 与内存字体状态语义

## 测试基线（x / y）

| 套件 | 结果 | 说明 |
| --- | ---: | --- |
| `npm test` | **145 / 169** | 24 skipped，0 fail |
| `npm run test:ops` | **50 / 50** | appservice 操作流回放 |
| `npm run test:page` | **14 / 14** | 合成 Skyline 页面 |
| `npm run test:l5` | **2 / 2** | Linux 确定性基线 + Windows 容差比较 |

## 维护方式

1. 完成任务后把对应 `[ ]` 改为 `[x]`。
2. 同步更新该分组的 `x / y`、顶部“核心/长尾/全部”计数与总览表。
3. 新增任务时分配下一个编号（`A#`/`B#`/`C#`），并更新总数。
4. 若任务被外部条件阻塞，保留 `[ ]` 并标注 `blocked`，不要计入已完成。
5. 每次阶段提交后在“最后更新”写入日期，测试基线按最新 `npm test` 结果更新。

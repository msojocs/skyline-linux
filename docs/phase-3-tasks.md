# 阶段 3 任务清单

> 最后更新：2026-11-20
> 分支：`feature/phase3-gesture-list-image-channels`
> 进度格式：**x / y** = 已完成任务 / 总任务
>
> 核心验收：**22 / 22（100%）**
> 长尾/外部阻塞：**7 / 12**
> 全部任务：**29 / 34（85.3%）**
>
> 勾选项表示已在 Linux 回放运行时上实现并通过测试；标有 `blocked` 的任务
> 依赖外部环境或私有引擎能力。

## 进度总览

| 分组 | 已完成 | 总数 | 进度 |
| --- | ---: | ---: | ---: |
| A. 原始阶段 3 验收项 | 8 | 8 | 100% |
| B. 后续运行时增量 | 14 | 14 | 100% |
| C. 长尾 / 外部阻塞 | 7 | 12 | 58.3% |
| **合计** | **29** | **34** | **85.3%** |

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
  - 私有文本编码标志已按 C2 落地。
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

## C. 长尾 / 外部阻塞（7 / 12）

- [x] **C1** 官方 Windows appservice guest 的真实 L5 采集与像素对比
  - `tools/l5-windows.js` 在 Wine + 官方 Windows Electron 中以 dpr=2 运行
    `skyline.node`，窗口就绪后经 `notifyAppLaunch` + `PageContext` 回放夹具操作，
    从共享内存读取 640x480 帧。原始 Windows 帧为 BGRA，工具统一转为 RGBA。
  - `spec/l5-baseline.win32.json` 保存 16 个夹具的 zlib 压缩帧、样本点与容差；
    `npm run test:l5` 在 Linux 以 dpr=2 重放同一夹具并逐像素比较。
  - 结果：10 个夹具逐像素完全一致，`shadow` 最大差 1；`overflow`/`fixed-scroll`
    失配率 < 0.2%（裁剪/滚动边缘）；`transform3d` 存在 3D 变换保真差异；
    `gridview`/`listview` 的合成 op 流无法触发官方 native builder 回调，标记为
    不可比。详见 `docs/feasibility.md` §3.1。
  - 采集命令：`npm run l5:windows`；复用已采集帧：`npm run l5:windows:reuse`。
- [x] **C2** 私有 `dart:ui` 文本标志
  - `dart:ui` 按官方恢复源码的位布局编码：TextStyle bit 20–22
    （`lineHeight` / `ignoreClusterLetterSpacing` / `useCoreTextWordBreak`），
    ParagraphStyle bit 13–15（`lineHeight` / `ignoreClusterLetterSpacing` /
    `backgroundUseLineMetrics`）；`lineHeight` 另经 `ParagraphBuilder::Create`/
    `pushStyle` 单独传入。
  - `paragraph_builder.cc` 解码到 `txt` 样式；`paragraph_builder_skia.cc` 再把
    `lineHeight`（px → 字号倍数）、两个布尔标志与 `backgroundUseLineMetrics`
    写入 SkParagraph。
  - SkParagraph 在 `backgroundUseLineMetrics` 下用完整行框绘制 span 背景与选区，
    `useCoreTextWordBreak` 保留给 CoreText 分词平台（Linux/Windows 与官方
    Windows 一致，继续使用 ICU line breaker）。
  - `npm run test:ui-compat` 用 40px strut + 10px run 的两段段落验证：开启标志时
    蓝色背景覆盖整行框，关闭时仅覆盖字体度量区域。
- [x] **C3** `HandleNativeViewPointerEvent` / native view 通道
  - `tag=="texture"` 节点经 `NewTexture(id, type, useTexture)` 物化为
    `SkylineTextureView`，按 `viewId` 注册到 native view 表；`TextureDispose`
    在节点移除/重建时释放 Dart 通道状态。
  - Linux 兼容层为 `SkylineTextureView` 增加无平台视图的占位渲染路径
    （黑色 slot + `RenderPointerListener`），并让 `kTexture` YGNode 将命中测试
    转发给 texture 子节点。
  - `HandleNativeViewPointerEvent` 解析 Dart `NativePointerEvent`，按 `viewId`
    定位节点并在 `SKYLINE_APP_SERVICE_DEBUG` 下输出 down/move/up 与局部坐标；
    Linux/Windows 无 CoreText 原生视图内容时以占位 slot 呈现。
  - `npm run test:ops` 新增 native view 指针转发与释放两个用例（52 / 52）。
- [x] **C4** `flex-basis` / `flex-shrink`
  - `flex` 简写支持 `<grow> [<shrink>] [<basis>]`、`none`/`auto`/`initial`；
    长手 `flex-grow` / `flex-shrink` / `flex-basis` 均生效。
  - `flex-basis` 支持 px、百分比、`auto`/`content` 与 `0`；百分比按父内容主轴
    解析，主尺寸未定时回退为内容尺寸。
  - 溢出时按 `flex-shrink * base` 的 CSS 缩放因子收缩；未显式声明 shrink 的
    页面保持原有不收缩布局，scroll-view 内容不参与收缩。
  - 新增 L5 夹具 `appservice-flex-basis.ops.json`（grow/basis、1:3 shrink、
    25% basis 三行），与官方 Windows 帧逐像素一致（maxDelta=0）。
- [x] **C5** `background-position` / `object-position`
  - 支持 1/2/4 值 CSS `<position>` 语法：关键字（left/right/top/bottom/
    center）、百分比、px，以及 `right 10px` / `bottom 20%` 边距形式。
  - `background-position` 默认 `0% 0%`，作用于 `background-size` 后的图像；
    repeat 模式下移动 tile grid 原点。
  - `object-position` 默认 `50% 50%`，作用于 `object-fit` 的 contain/cover/
    none/scale-down 结果；`<image>` 的 `objectPosition` 属性也可覆盖。
  - `test:ops` 新增 4 个 contain 盒（object-position left/right、background-
    position center/right-bottom）像素用例（54 / 54）。
- [x] **C6** `box-sizing`（`content-box`）
  - `box-sizing: border-box|content-box` 解析；默认保持 Skyline 现有的
    border-box 行为，`content-box` 显式 opt-in。
  - content-box 下 `width`/`height`/`min/max-*`（px/%）按内容盒解析，布局统一
    换算为 border-box；padding/border 不再压缩内容。
  - 子节点定位、flex base size、absolute 尺寸与文本换行宽度均使用换算后的
    border-box。
  - `test:ops` 新增 content-box / border-box 对照用例（55 / 55）；
    L5 夹具 `appservice-box-sizing.ops.json` 与官方 Windows 帧逐像素一致
    （maxDelta=0）。
- [x] **C7** 百分比 `padding` / `margin`
  - `padding`/`margin` 的 1–4 值简写与 `-top/right/bottom/left` 长手均保留
    百分比；px 值维持原行为。
  - 按 CSS 规则统一以**包含块宽度**解析（上下左右轴都是），在测量阶段写入
    布局使用的 padding/margin；box-sizing、flex base/shrink、absolute 尺寸与
    scroll/builder padding 均使用解析后的值。
  - `test:ops` 新增 `padding:10%`（320px 包含块 → 32px）与
    `margin:10% 25%`（200px 包含块 → 20px/50px）像素用例（56 / 56）。
  - L5 夹具 `appservice-percent-box.ops.json` 与官方 Windows 帧逐像素一致
    （maxDelta=0）。
- [ ] **C8** `position: sticky`
- [ ] **C9** radial / conic gradient
- [ ] **C10** 显式 `background-size` 长度值（非 cover/contain）
- [ ] **C11** `transform-origin` 的 z 分量 / 3D origin
- [ ] **C12** `AddPendingFont` / `RemovePendingFont` 与内存字体状态语义

## 测试基线（x / y）

| 套件 | 结果 | 说明 |
| --- | ---: | --- |
| `npm test` | **151 / 175** | 24 skipped，0 fail |
| `npm run test:ops` | **56 / 56** | appservice 操作流回放 |
| `npm run test:page` | **14 / 14** | 合成 Skyline 页面 |
| `npm run test:l5` | **2 / 2** | Linux 确定性基线 + Windows 容差比较 |

## 维护方式

1. 完成任务后把对应 `[ ]` 改为 `[x]`。
2. 同步更新该分组的 `x / y`、顶部“核心/长尾/全部”计数与总览表。
3. 新增任务时分配下一个编号（`A#`/`B#`/`C#`），并更新总数。
4. 若任务被外部条件阻塞，保留 `[ ]` 并标注 `blocked`，不要计入已完成。
5. 每次阶段提交后在“最后更新”写入日期，测试基线按最新 `npm test` 结果更新。

# 阶段 3 任务清单

> 最后更新：2026-11-22
> 分支：`feature/phase3-gesture-list-image-channels`
> 进度格式：**x / y** = 已完成任务 / 总任务
>
> 核心验收：**22 / 22（100%）**
> 长尾/外部阻塞：**12 / 12（100%）**
> 全部任务：**34 / 34（100%）**
>
> 勾选项表示已在 Linux 回放运行时上实现并通过测试；标有 `blocked` 的任务
> 依赖外部环境或私有引擎能力。

## 进度总览

| 分组 | 已完成 | 总数 | 进度 |
| --- | ---: | ---: | ---: |
| A. 原始阶段 3 验收项 | 8 | 8 | 100% |
| B. 后续运行时增量 | 14 | 14 | 100% |
| C. 长尾 / 外部阻塞 | 12 | 12 | 100% |
| **合计** | **34** | **34** | **100%** |

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

## C. 长尾 / 外部阻塞（12 / 12）

- [x] **C1** 官方 Windows appservice guest 的真实 L5 采集与像素对比
  - `tools/l5-windows.js` 在 Wine + 官方 Windows Electron 中以 dpr=2 运行
    `skyline.node`，窗口就绪后经 `notifyAppLaunch` + `PageContext` 回放夹具操作，
    从共享内存读取 640x480 帧。原始 Windows 帧为 BGRA，工具统一转为 RGBA。
  - `spec/l5-baseline.win32.json` 保存 25 个夹具的 zlib 压缩帧、样本点与容差；
    `npm run test:l5` 在 Linux 以 dpr=2 重放同一夹具并逐像素比较。
  - 结果：14 个夹具逐像素完全一致，`shadow`/`radial-conic`/`background-size`
    最大差 1；`overflow`
    失配率 < 0.2%（裁剪边缘），`fixed-scroll` 已完全一致；`transform3d` 存在 3D 变换保真差异；
    `transform-origin-z` 因官方引擎忽略 origin z 分量而失配率约 10%（已知差异）；
    D2 探针新增的 `aspect-ratio`/`vertical-align`/`text-overflow`/`white-space`
    为已知差异，`map` 逐像素一致；`gridview`/`listview` 的合成 op 流无法触发
    官方 native builder 回调，标记为不可比。详见 `docs/feasibility.md` §3.1。
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
    `margin:10% 25%`（200px 包含块 → 20px/50px）像素用例（57 / 57）。
  - L5 夹具 `appservice-percent-box.ops.json` 与官方 Windows 帧逐像素一致
    （maxDelta=0）。
- [x] **C8** `position: sticky`
  - CSS `position: sticky` 解析为 position=3，节点仍参与正常 flex 流布局。
  - 滚动容器检测 sticky 子节点：直接子节点包装为 `NewStickyHeader` 并统一放入
    `NewStickySection`；包含 sticky 子节点的 section 容器整体物化为
    `StickySection` sliver（滚动类型切换为 CustomScrollView）。
  - `top` inset 作为 pinned header 的 offsetTop；`onStickOnTopChange` 回调输出
    调试状态。`bottom`/水平 sticky 语法已解析，当前按 top 行为处理。
  - `test:ops` 新增 sticky header + 400px body 滚动用例（57 / 57）；
    L5 夹具 `appservice-sticky.ops.json` 与官方 Windows 帧逐像素一致
    （maxDelta=0）。
- [x] **C9** radial / conic gradient
  - `ParseCssGradient` 统一解析 `linear-gradient` / `radial-gradient` /
    `conic-gradient`；径向支持 `circle`/`ellipse`（引擎为单半径，按圆处理）、
    `closest-side`/`farthest-side`/`closest-corner`/`farthest-corner` 与显式
    长度，`at <position>` 复用 1/2/4 值位置语法（关键字/%/px）；锥形支持
    `from <angle>`（deg/rad/turn）与 `at <position>`，颜色停靠点可为角度或百分比。
  - 颜色停靠点支持 0/1/2 值位置（`red 0deg 90deg` 生成硬停止），未指定位置按
    现有规则均匀分布；百分比与 px 停靠点分别以 `GradientStop.isPercent` 传入。
  - 按类型调用 `NewRadialGradient` / `NewConicGradient`，`SameGradient` 比较
    类型、中心/半径/尺寸关键字与停靠点；更新仍走 `DecoratedBoxUpdateGradient`。
  - `test:ops` 新增 100×100 圆径向（中心红、外沿蓝）、30px 显式半径径向、
    四象限硬停止锥形、`from 90deg` 旋转锥形，以及径向→锥形原地更新用例
    （59 / 59）。
  - L5 夹具 `appservice-radial-conic.ops.json` 与官方 Windows 帧比较
    maxDelta=1、失配率 0。
- [x] **C10** 显式 `background-size` 长度值（非 cover/contain）
  - `background-size` 支持 1–2 个值：`auto`、px、百分比（相对背景定位区）；
    长手与 `background` 简写（`<position>/<size>`、含无空格 `center/50px`）均解析。
    值写入 `AppStyle`，`cover`/`contain` 继续走 `background_fit`。
  - 背景图由 `BackgroundImagePaint` 原生绘制：显式尺寸按 CSS 规则解析（单值 +
    `auto` 保持位图宽高比、双值精确缩放），重复时以该尺寸为 tile，并按背景
    定位区裁剪；`background-position`/`repeat` 同步生效。
  - 线性渐变经 `NewLinearGradient` 的 fittingSize/alignment/side/offset/repeat
    参数应用同一套尺寸与位置；并修正 CSS repeat 到 Flutter `ImageRepeat`
    枚举序（repeat=0…noRepeat=3）的映射。径向/锥形 FFI 无尺寸参数，不受影响。
  - 更新路径：URL 变化触发重建，尺寸/位置/repeat 变化经
    `BackgroundImageMarkNeedPaint` 重绘背景图、经 `DecoratedBoxUpdateGradient`
    更新渐变。
  - `test:ops` 新增 4 种图片尺寸（40×20、50%、`32px auto` 平铺、`auto 40px`
    右下锚定）与图片/渐变尺寸原地更新用例（61 / 61）。
  - L5 夹具 `appservice-background-size.ops.json`（px/%/平铺/简写四种线性
    渐变）与官方 Windows 帧比较 maxDelta=1、失配率 0。
- [x] **C11** `transform-origin` 的 z 分量 / 3D origin
  - `ParseTransformOrigin` 解析可选的第三分量（px，默认 0），写入
    `AppStyle::transform_origin_z`；重新声明 origin 时 z 归零。
  - YGNode 的 Dart 侧只承载 2D `Offset` origin，因此原生侧把 z 折叠进矩阵：
    `T(0,0,z) · M · T(0,0,-z)`；它与 widget 的 2D origin 平移组合后等价于
    完整的 3D 枢轴，无需修改重建包/FFI 签名。
  - 首次物化与 `transform`/origin 的 targeted update 均使用折叠后的矩阵；
    `transform_origin_z` 变化会触发原地更新。
  - `test:ops` 新增 `perspective(300px) rotateX(40deg)` 下 z=0 与 z=60px 的
    投影差异用例，以及 z 分量原地更新用例（63 / 63）。
  - 官方 Windows 引擎忽略 origin z（同一 transform 下 z=0/60 帧完全相同），
    3D 投影本身也存在既有保真差异；L5 夹具 `appservice-transform-origin-z`
    记录为已知差异（失配率约 10%）。
- [x] **C12** `AddPendingFont` / `RemovePendingFont` 与内存字体状态语义
  - 原生运行时维护内存字体状态：`g_pending_fonts`（加载中的 family）、
    `g_font_callback_families`（LoadFontFace 回调 id → family）与
    `g_font_status`（最近一次状态；-1 = loading）。
  - `AddPendingFont` 登记 family 并标记 loading；`RemovePendingFont` 移除并检查
    当前页面是否有文本引用该 family，若有则通过 UI runner 定位重排这些文本
    （`TextApplyUpdate` + `MarkYGNodeNeedsLayout`），不重建页面、不重复触发
    @font-face 加载。`NotifyFontFaceLoad` 以 family 维度记录最终状态并输出日志。
  - 新增 CSS `font-family` 解析（含引号与逗号列表）与继承；由于 NewTextSpan
    只有 `fontFamilyFallback`，CSS family 置于回退链最前，其后追加 CJK/系统
    回退。`font-family` 变更触发重建，字体加载完成走定位重排。
  - JSON 页面描述符的 text 节点支持 `fontFamily`。
  - `test:ops` 新增真实系统字体加载 + 定位重排用例，并扩展伪字体用例覆盖
    pending/relayout 状态（64 / 64）。

## D. Windows 原版对照缺口（后续任务，未计入阶段 3 总数）

> 入口：`npm run gaps`（`tools/conformance-gaps.js`）生成当前缺口报告。
> 这些任务把“静态候选/固定夹具”升级为可判定的 Windows 差分。

- [x] **D1** 补录 24 个 engine-context 契约用例（Wine 渲染进程 oracle）
  - 新增 `tools/win-oracle-app/`：在 Wine 官方 Electron 的渲染进程中加载官方
    `skyline.node`，复用 `tools/l5-win-app` 的窗口初始化流程（共享内存句柄、
    `notifyAppLaunch`、全套回调）。`test/lib/win-shim.js` 把
    `resources/app` 替换为 oracle 宿主，绕过官方 Electron “已打包应用忽略
    CLI app 路径”的限制。
  - `test/lib/runner.js` 的 win32 分支在 `allowEngineContext` 时走渲染进程，
    其余用例仍用 `ELECTRON_RUN_AS_NODE` + `test/driver.js`；`test/driver.js`
    导出 `executeCase()` 供渲染进程复用。
  - `spec/skyline-node.win32.json` 现为 60 个用例：50 ok，10 官方进程崩溃
    （`device_pixel_ratio == 2`、AOT 路径、无效 descriptor 等；记录归一化
    `reason`，完整日志留在 `tmp/results/`）。新增 14 个可比用例
    （brightness/destroy/notify/create 数值字符串等）。
  - 顺带修正 addon 行为以匹配官方：`createWindow` 对 windowId/width/height/dpr
    做 `Number()` 化；`destroyWindow` 校验 1 个参数并对未知窗口抛
    `window_id: N not found`；`onPlatformBrightnessChanged` 校验 1 个参数。
  - 结果：`native/build/skyline.node` 契约 **53 pass / 10 skip / 0 fail**
    （10 个 skip 是官方进程崩溃、无稳定基准的用例）。
- [x] **D2** 为候选 CSS/内置标签生成 L5 探针夹具
  - 新增 5 个夹具：`appservice-aspect-ratio`、`appservice-vertical-align`、
    `appservice-text-overflow`、`appservice-white-space`、`appservice-map`，
    加入 `tools/l5-baseline.js` FIXTURES 与 Linux/Windows 基线。
  - `tools/l5-windows.js` 增加 `--only name1,name2` 合并模式：只采集新夹具并保留
    已有 Windows 帧，避免整份基线重采。
  - 实测差异（官方帧 vs Linux，dpr=2）：
    - `aspect-ratio`：官方 100x50，Linux 高度 0，失配 3.48%；
    - `vertical-align`：官方 inline-block 按 top/bottom/middle 排一行，Linux 竖排，
      失配 6.31%；
    - `text-overflow`：官方 `nowrap + ellipsis`，失配 1.37%；
    - `white-space`：官方区分 `nowrap`/`normal`，失配 1.93%；
    - `map`：maxDelta=0（该 op 流下两边都是空白占位，原生地图内容未被覆盖）。
  - 四个文本/布局差异中 `aspect-ratio` 已在 D6 修复并移出
    `KNOWN_DIVERGENCES`；其余三个保留说明与 0.5% headroom。
    `pointer-events` 已在 D3 实现并加行为探针；`will-change` 无行为差异。
- [x] **D3** 候选回调行为探针
  - 产出 `docs/callback-coverage.md`：按“已接线 / 其他路径覆盖 / 确认缺失”
    分类候选回调，并给出源码 0 引用证据与影响面。
  - 实现 `pointer-events: none`：解析后传 `PointerEvents.none` 给 `NewYGNode`，
    `SkylineGetNodeRect` 增加 `pointerEvents` 字段，appservice 镜像 hit test
    跳过该子树。
  - 新增两个 `test:ops` 行为探针：`pointer-events:none` 覆盖层可穿透滚动；
    同几何、无 `pointer-events` 的对照组覆盖层会拦截滚动。
  - `will-change` 确认无静态/行为差异，不解析、不设探针。
  - 确认缺口（D7/D8/D9/D10 已分别补齐滚动、Swiper/PickerView/Input、
    IntersectionObserver/Selection/Refresher/返回拦截、用户手势/无障碍语义/
    自动化快照与内存 API）：无剩余行为候选。
  - 回归：`test:ops` 70/70（含 2 个 pointer-events 探针）；顺带把
    “移除背景” 用例的 200ms 更新改为 600ms，消除初始物化竞态。
- [ ] **D4** 真实页面 ops 录制与整页差分
  - Linux DevTools 与 Windows 原版使用同一份官方 renderer bundle；把 Linux
    侧 `applyPageOps` 的真实 ops 录下来，喂给 Wine 官方引擎与 Linux 引擎，
    对 `awesome-skyline/examples` 做整页像素/树结构对比。
- [ ] **D5** 把 `npm run gaps` 纳入基线
  - 固定报告格式，加入 CI：或提交 `docs/conformance-gaps.md` 基线，
    在契约/L5 结果变化时显式更新。
- [x] **D6** 修复 D2 探针确认的 CSS/布局差异
  - `aspect-ratio`：解析 `auto`/`n`/`n / m`，在确定轴上推导另一轴（含
    border-box/content-box、min/max 与 flex stretch 语义）。L5 探针现与官方
    逐像素一致（maxDelta=0）。
  - `display:inline-block`：块容器内全部为行内级子项时按行盒排布，行高取最大
    子项高度并底部对齐；`vertical-align` 探针的三个色块位置与官方完全一致，
    剩余 ~0.8% 为文本栅格化差异。
  - `white-space`：支持 `normal`/`nowrap`/`pre`/`pre-wrap`/`pre-line`；
    `NormalizeTextWhitespace` 按模式保留/折叠空白；nowrap 不换行，块级文本按
    容器宽度换行。`text-overflow` 支持 `clip`/`ellipsis`（映射 Flutter
    TextOverflow）。
  - 文本宽度：块级文本节点按包含块宽度约束，使 nowrap + ellipsis 能在容器内
    裁剪；行内流中的文本保持固有宽度。
  - 已知剩余：Skyline 默认 `WordBreak.breakAll` 未能在该 Flutter 文本路径上
    生效（U+200B 试验无效），`white-space` 探针剩余 ~2.3% 为断行/栅格化差异；
    `text-overflow` 在该 op 流下官方也是直接裁剪，剩余 ~1.6% 为文本栅格化。
    L5 基线已记录说明。
  - 回归：`test:ops` 68/68、`test:page` 14/14、`test:l5` 2/2 通过。

- [x] **D7** 滚动事件回调
  - `NewScrollView` 传入真实的 `ScrollStartEvent`/`ScrollEvent`/`ScrollEndEvent`，
    创建后再通过 `ScrollViewUpdateOnScrollStart/OnScroll/OnScrollEnd` 安装同一组
    回调（`SKYLINE_APP_SERVICE_DEBUG` 下输出
    `scroll callbacks installed #<id> (start=1 scroll=1 end=1)`）。
  - 滚动回调更新节点的 `scrollTop` 属性（后续 setData 重建不会跳回顶部），并经
    `SkylineSetScrollEventSink` 把位置/尺寸/增量/isDrag/边界速度投递到 addon 的
    JS 线程。
  - bootstrap 新增 `native.setScrollSink` 分发：普通 `bindscroll`
    （`node.onScrollEvent`）与 worklet（`ReanimatedModule.registerEventHandler`
    的 `onScroll-<tag>`）两条路径都收到
    `{type, detail:{scrollTop, scrollLeft, scrollWidth, scrollHeight, deltaX, deltaY}}`；
    `getScrollOffset` 返回最近一次真实偏移。
  - 修复 `PageContext.render(callback)`：appservice SDK 的
    `setData(data, callback)` 走 `triggerRender -> backendContext.render(cb)`，
    之前忽略回调，address-book 的 `computedSize()` 因此不执行、`_tops` 保持
    2e10 sentinel，滚动高亮永不更新。
  - 验证：新增 `test/bootstrap-scroll.test.js` 覆盖 sink→bindscroll/worklet 分发与
    render 回调；`test:ops` 断言三个 Update* 回调均已安装（70/70）；Linux
    DevTools 真机 address-book 滚轮滚动后右侧 A-Z 高亮由 A 切换为 B。
- [x] **D8** Swiper/PickerView/Input 编程式 API 与事件
  - `swiper` 改用真实 `NewSwiper`（页面型 BetterSwiper）并安装
    `SwiperUpdateOnChange` / `SwiperUpdateOnScroll`；change/scroll 回调更新
    `current` 并经 `SkylineSetWidgetEventSink` 投递到 appservice，bootstrap 分发给
    内置组件的 `onChangeEvent` / `onScrollEvent` 与 `onChange`/`onTransition`
    worklet。`SwiperAnimateToPage` / `SwiperJumpToPage` 由 `current` 属性更新、
    `swiperTo` op 与 `native.swiperTo` 调用（`duration:0` 自动走 jump，规避 Flutter
    `duration > Duration.zero` 断言）。
  - `picker-view-column` 改用真实 `NewPickerViewColumn`，安装
    `PickerViewColumnUpdateOnChange/OnStart/OnEnd`；`value` 属性变化、
    `pickerTo` op 与 `native.pickerColumnSetCurrent` 调用
    `PickerViewColumnUpdateCurrent`。change 事件会回填各列 `value` 并合成
    picker-view 的 `{value:[...]}` detail。
  - `focusInput`/`blurInput` 与 `focus` 属性变化调用
    `InputFocusManually` / `InputBlurManually`（真实 NewInput 句柄）；新增
    `native.inputFocus` 与 `__skylineDebug.inputFocus`。
  - 回归/验证：新增 3 个 `test:ops` 用例（swiper 换页 + change/scroll 回调、
    picker 列回调 + 编程式选中、input 焦点/失焦）；`test:ops` **73 / 73**；
    bootstrap 单测覆盖三条 JS 分发路径。Linux DevTools `tab-indicator`：点击
    第二个 Tab 后 swiper 换页，`__skylineDebug.swiperTo(...,2)` 触发
    `bindchange` 使第三个 Tab 选中；album 预览页 swiper 正常渲染。
- [x] **D9** IntersectionObserver / Selection / Refresher / 返回拦截
  - **IntersectionObserver**：`setIntersectionListener` 经
    `native.addIntersectionObserver` 调用 `YGNodeAddIntersectionObserver`（真实
    Dart YGNode），`unobserve/disconnect` 调
    `YGNodeRemoveIntersectionObserver`；Dart 层阈值/可见性变化经导出的
    `YogaTriggerIntersection(Observers)` 回传 rects/ratio/isIntersecting（注意这
    三个导出在 kernel lookup 中使用 **Float**，不是 JSON typedef 里的 Double），
    按 JSON 通道分发给 JS observer callback。支持 DOM 镜像与
    `__skylineDebug.intersectionEvents`；同时实现 SDK 旧路径
    `node.createIntersectionObserver(root, margins, thresholds, cb)`（cb 收到
    单元素数组）。
  - **Selection**：`<selection>` 节点创建真实 `NewSelection`（`disableContextMenu`、
    句柄/选区颜色），安装 `SelectionUpdateOnSelectionChange`，change 事件携带
    `selectedString` 与包围盒；`selectionSetRange`/`selectionRange` op 以文本 span
    地址（`node->address`）调用 `SelectionSetRange`；selection 子树内文本自动
    `userSelect=true`，因此原生长按选区可用。
  - **Refresher**：`NewScrollView` 映射 `refresher-enabled/threshold/background/
    default-style/triggered` 与二级阈值（`twoLevelThreshold` 默认 150 / close 80）；
    接入 `onRefresherPulling/WillRefresh/Refresh/Restore/Abort/StatusChange` 原生
    回调并分发给内置 scroll-view 的对应回调；`refresherTriggered` 属性变化、
    `refresherTrigger` op、`native.refresherTriggered` 调用
    `ScrollViewRequestRefresherRefresh/TwoLevel`。
  - **返回拦截**：`setNavigateBackInterception`/`native.setNavigateBackInterception`
    调用 `SetNavigateBackInterception(pageId, enabled)`；Dart 路由在拦截手势时
    调用导出的 `OnNavigateBackIntercept`，经 JSON 通道通知 bootstrap
    （`__skylineDebug.navigateBackIntercepts`，并尝试回放
    `onNavigateBackIntercept` 事件）。
  - 回归/验证：`test:ops` 新增 2 个用例（refresher 编程式下拉触发 pulling/
    status/refresh；intersection 注册+阈值触发、selection setRange/change、返回
    拦截）；bootstrap 单测覆盖 intersection/selection/refresher/navigate-back 分发。
    真机（release）：`wx.createIntersectionObserver` 页面收到
    `intersectionRatio=1/isIntersecting=true`（rootBounds 为 viewport）；
    编程式 refresher 触发 `refreshCount=1`、status refreshing→restore。
- [x] **D10** 无障碍与 DevTools 自动化
  - **语义树**：appservice 节点上的 `aria-label` / `aria-role` / `aria-hidden` /
    `aria-disabled`（及 `semantics*` 别名）解析为 `SemanticsInfo`。首建时经真实
    `NewSemantics` 包住元素 widget（`SkylineSemantics`），属性增量更新走
    `SemanticsLabel` / `SemanticsRole` / `SemanticsHidden` / `SemanticsDisable`
    原地刷新；新增/移除包装需要重建时置 `force_rebuild`。
  - **无障碍状态**：实现 `UpdateAccessibility(runtime, enabled)` 与
    `OnAccessibilityFeaturesChanged(runtime, boldText)`（原来都是 void stub）。
    运行时保存 `enabled/boldText`，调用注册的 `AccessibilityBoldTextEnabled`，
    并经新增的 `SkylineSetAccessibilitySink` 通知宿主。`host/main.cc` 与
    `host/host_engine.cc` 在 **platform 线程** 调用
    `FlutterEngineUpdateSemanticsEnabled` / `FlutterEngineUpdateAccessibilityFeatures`。
    由于宿主不使用 fml 消息循环，语义更新会挂在 platform task runner 上，循环内
    用 `__FlutterEngineFlushPendingTasksNow()` 显式排空；否则
    `update_semantics_callback2` 永不触发。`SKYLINE_APP_SERVICE_DEBUG` 下宿主
    逐节点输出 `semantics node: id=.. label=".." button=..`。
  - **快照**：`SceneToImage` 用 `Scene::takeLayerTree` → `LayerTree::Flatten` →
    `DlSkCanvasDispatcher` 在 CPU SkSurface 上光栅化，PNG 走 `SkPngEncoder`，
    其余格式回读 RGBA；回调收到 `Uint8List`。`<snapshot>` 元素经 `NewSnapshot`
    包装并支持 `SnapshotTakeSnapshot`；`TakeAppSnapshot` 包装 Dart 的
    `RenderRepaintBoundary.toImage`；`YGNodeTakeSnapshot` 按需把节点
    `setIsRepaintBoundary(true)` 后截图。结果写入 `SKYLINE_SNAPSHOT_DIR`
    （`snapshot-app-<id>.png` / `snapshot-node-<id>.png`）并作为
    `appSnapshot`/`nodeSnapshot` JSON 事件投递到 JS。`<snapshot>` 的
    `allowSnapshotting` 默认关闭：开启后 Dart `SnapshotWidget.toImageSync`
    在无 GPU 上下文的软件宿主上会触发 raster-thread 断言，自动化请使用
    `snapshotNode`（内部按需设 repaint boundary）。
  - **自动化**：`DumpElementTree` / `DumpRenderTree` / `CollectGarbage` /
    `SendMessageToRunner` / `RelaunchApp` / `ShutdownApplication` /
    `StartUserGesture` / `StopUserGesture` 均通过 `FindCallback` 调用真实 Dart
    回调，并由 appservice ops（`dumpElementTree`、`collectGarbage`、
    `sendMessageToRunner`、`takeAppSnapshot`、`snapshotNode`、`userGesture`、
    `accessibility`、`relaunchApp`、`shutdownApplication`）与 addon
    `__skylineDebug.*` 暴露给 DevTools。
  - 回归/验证：新增 `test:ops` 用例覆盖语义树、宿主语义节点、in-place 标签
    更新、两类快照、树 dump、GC、runner 消息与用户手势；`test:ops` **76 / 76**。
    bootstrap 单测覆盖全部 `__skylineDebug` 自动化方法与 JSON 事件分发；
    `npm test` **176 / 200**（24 skipped）。

## 测试基线（x / y）

| 套件 | 结果 | 说明 |
| --- | ---: | --- |
| `npm test` | **176 / 200** | 24 skipped，0 fail |
| `npm run test:ops` | **76 / 76** | appservice 操作流回放 |
| `npm run test:page` | **14 / 14** | 合成 Skyline 页面 |
| `npm run test:l5` | **2 / 2** | Linux 确定性基线 + Windows 容差比较 |

## 维护方式

1. 完成任务后把对应 `[ ]` 改为 `[x]`。
2. 同步更新该分组的 `x / y`、顶部“核心/长尾/全部”计数与总览表。
3. 新增任务时分配下一个编号（`A#`/`B#`/`C#`），并更新总数。
4. 若任务被外部条件阻塞，保留 `[ ]` 并标注 `blocked`，不要计入已完成。
5. 每次阶段提交后在“最后更新”写入日期，测试基线按最新 `npm test` 结果更新。

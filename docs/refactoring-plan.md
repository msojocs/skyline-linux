# 重构方案

> 日期：2026-11-26　基线提交：`4c0c397`
>
> 范围：`native/`（addon、host、runtime、引擎脚本、shared-memory）、`test/`、
> `tools/`、`spec/`、`docs/` 与 CI。本文只做问题分析与实施规划，不包含代码改动。
> 文中“证据”均为基线提交上的 `文件:行号`，数字为同一提交上的统计。

## 0. 摘要

项目已经从“可行性探索”演进为可以在微信开发者工具中实际使用的 Linux Skyline
实现，但工程结构仍停留在探索期：功能按“修一个页面问题 → 加一段代码 + 一个测试”
线性堆叠，缺少分层、产物一致性校验和能快速运行的回归网。主要结论：

1. **安全网已经失效（最紧急）**。c110f03 之后，host 侧测试整体变红（Dart 端
   `DynamicLibrary.open('stub')` 失败），但 CI 只跑在缺少 debug 引擎时会自动跳过的
   `npm test`，因此没有任何信号；L5 比较对缺失的夹具或基线条目静默 `continue`。
2. **构建产物没有指纹**。运行时编进 `libflutter_engine.so`，Dart 补丁编进
   kernel/AOT，但各步骤之间只检查“文件存在”。改了 `native/runtime` 却没有在两种
   模式下重新嵌入时，测试和 staging 会静默使用旧运行时。CI 发布包则把仓库最新的
   `skyline.node` 与手工上传、来源不明的 `build-deps`（引擎 + AOT）拼在一起。
3. **原生运行时是单体结构**。17.8k 行 C++ 共享一个“上帝头文件”和约 50 个可变全局量
   （只有 2 把锁）。`BuildWidget` 1062 行，`LayoutNode` 677 行，`ApplyOperation`
   556 行，`ApplyDeclaration` 498 行；`FindCallback` 有 210 个调用点；录制回放和实时
   回放是两份平行实现；CSS、布局这类纯逻辑无法脱离引擎测试。
4. **边界没有契约**。addon 手写了 34 个 `extern "C"` 声明，host 另写一组 weak
   extern，`Engine` 抽象只覆盖 6 个操作。bootstrap JS 是一个被切成 14 片、各片不能
   单独解析的 157 KB 闭包，含 31 个空 `catch {}` 和 5 个常驻轮询定时器。
5. **文档与现状漂移**。README 中的测试计数、夹具数，以及“引擎为 stub”等描述都已
   过期；没有架构文档，也没有“改了 X 要重建 Y”的说明，这些知识目前只存在于提交
   记录里。

建议按“先止血、再建安全网、后拆结构”的顺序分 7 个阶段推进（§6）。所有重构步骤
都以**行为不变**为验收标准：结构化布局快照（golden，P2 新增）逐字节一致，且与
Windows 基线的 L5 比较结果不变。

| 阶段 | 主题 | 规模（人日，粗估） | 依赖 |
|---|---|---|---|
| P0 | 止血：修复回归、恢复全绿、L5 集合校验 | 1–2 | — |
| P1 | 构建流水线：产物指纹、过期检测、CI 发布一致性 | 4–6 | P0 |
| P2 | 测试基础设施：统一 host harness、布局 golden、拆分 ops 测试 | 5–8 | P0 |
| P3 | 边界：运行时公共 API 头、addon 绑定、host 公共层、配置与日志 | 5–8 | P2 |
| P4 | 运行时内核：回调表、数据模型、core 库抽取、拆分巨型函数 | 15–25 | P2，部分依赖 P3 |
| P5 | Bootstrap JS 模块化 | 5–8 | P2（可与 P4 并行） |
| P6 | 文档与开发体验 | 3–5（持续） | 随各阶段 |

## 1. 范围、规模与约束

### 1.1 代码规模

| 组件 | 位置 | 规模 |
|---|---|---|
| 运行时（回放 + FFI） | `native/runtime/` | 17 个 `.cc` + 18 个内部头，约 17.8k 行 |
| Addon | `native/src/main.cc` | 1619 行，44 个 `native.*` 方法 |
| Bootstrap JS | `native/src/bootstrap/` | 14 片，约 4.0k 行 |
| 宿主 | `native/host/` | `main.cc` 1012 行 + `host_engine.cc` 729 行 |
| 共享内存 | `native/shared-memory/` | 约 1.7k 行，另内置 node-addon-api |
| 引擎脚本 / 补丁 | `native/engine/` | 9 个脚本、4 个 diff 补丁、`app_compat/apply.py` 485 行 |
| 测试 | `test/`、`native/test/` | 约 256 个用例；`skyline-appservice-ops.test.js` 单文件 5284 行、103 个用例 |
| 夹具 / 基线 | `test/fixtures/`、`spec/` | 60 个 ops 夹具；`spec/l5-baseline.win32.json` 860 KB |
| npm 脚本 | `package.json` | 52 个 |

### 1.2 约束

- 官方 `skyline/*` 与定制引擎闭源；`package:skyline` 源码从官方 kernel 恢复，
  **不入库**（位于 `native/engine/work/`），只能以补丁形式修改。
- 引擎 checkout 与构建需要 20–30 GB；每次修改运行时都要在两种模式下重新链接引擎。
- 判定标准是与 Windows 官方引擎逐像素比较（`AGENT.MD`）；L5 Windows 基线需要
  Wine + 官方 Electron 采集。
- 仓库目前没有 npm 依赖、测试不联网；本方案保持这一约束（需要的测试框架以单头文件
  方式内置，与现有 node-addon-api 的做法一致）。
- 功能修复仍在密集进行（近 30 个提交都是 DevTools 页面修复），重构必须能与功能开发
  交错进行，不能依赖长期分支。

### 1.3 目标与非目标

目标：

- 任何改动之后都能在分钟级得到可信的回归信号；
- 构建产物可追溯，过期产物不会再静默生效；
- 运行时按职责分层，纯逻辑（CSS、选择器、布局、文本测量）可以脱离引擎测试；
- 跨线程、跨语言边界有显式契约；
- 文档与代码一致，新贡献者可以从文档推出“改了 X 要重建 Y”。

非目标：

- 不改变渲染行为（缺陷修复单独提交，不混入重构提交）；
- 不换语言或框架，不升级 Flutter 引擎版本；
- 不在本方案内追求新的官方行为对齐（D4/D5 等仍作为独立任务）。

## 2. 现状架构速览

```
 WeChat DevTools（Electron，appservice 渲染进程）
 ┌────────────────────────────────────────────────────────────────────┐
 │ glass-easel / Skyline SDK                                          │
 │     │ PageContext / BridgeElement 节点操作                         │
 │     ▼                                                              │
 │ bootstrap JS（native/src/bootstrap/*.js，单个闭包）                │
 │     │ native.applyPageOps / getNodeRect / swiperTo / ...           │
 │     ▼                                                              │
 │ skyline.node（native/src/main.cc）── 34 个手写 extern "C" ──┐      │
 │     │ Engine 接口（6 个操作）                                │      │
 │     ▼                                                        ▼      │
 │ host_engine.cc：每窗口一个 platform 线程     libflutter_engine.so   │
 │   FlutterEngineRun / vsync / 指针队列        ├─ Flutter 引擎         │
 │   帧 BGRA→RGBA → 共享内存 → TSFN             └─ 嵌入的 native/runtime│
 │                                                 （UI task runner）  │
 │                                                 ▲ FFI 73 个符号     │
 │                                                 │ 回调（stringHash）│
 │                                        package:skyline（kernel/AOT）│
 └────────────────────────────────────────────────────────────────────┘
```

运行时内部的数据流：ops JSON → `ApplyOperation`（`appservice_ops.cc:780`）建立
`AppNode` 树并收集 `CssRule`；attach 时 `MaterializeAttachedPage`
（`appservice_page.cc:74`）依次执行 `ResolveStyles` → `ComputeInlineWhitespace` →
`MeasureNode`/`LayoutNode`（自研 flexbox 子集，导出的 `YogaCalculateLayout` 是空
实现）→ 写入每个节点的 9-float 布局缓冲 → `BuildWidget` 递归调用 Dart 回调物化 →
`CreatePageRoute`。后续批次走 `ApplyTargetedUpdates`（样式 diff →
`*Update*` 回调）或整页 `RebuildAppServicePage`；`PublishNodeRects` 在锁内发布
几何快照，供 JS 侧命中测试使用。

线程与通道：

| 通道 | 方向 | 机制 |
|---|---|---|
| 页面 ops / 查询 | JS → runtime | addon 直接调用 `extern "C"`；runtime 经 `PostToUiIfReady` 切到 UI 线程，或在锁内读快照 |
| 输入 | JS → platform 线程 | `Engine::DispatchPointer/Wheel` → `PostPlatformTask` 队列 → `FlutterEngineSendPointerEvent` |
| 事件（scroll / widget / json / http） | UI → JS | runtime sink（原子指针）→ addon TSFN → bootstrap 分发 |
| 帧 | raster → JS | BGRA→RGBA 拷入共享内存 + `onWindowRasterized` TSFN + bootstrap 8 ms 补发泵 |
| 无障碍 | UI → platform | weak 符号 sink → 带锁队列 |
| vsync | engine → platform | 带锁 deque，由 platform 线程归还 baton |

## 3. 问题诊断

严重度：**高** = 已经或极易造成错误结果或静默失效；**中** = 明显拖慢开发或放大
回归风险；**低** = 卫生问题。

### 3.1 当前回归（P0）

> R1、R3 已在 P0 修复，R2 留给 P1，见 §6 P0 的状态说明。

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| R1 | host 侧测试整体失败。c110f03 让 Linux 上的 `SkylineApplicationDevtools` 用 `skylineAddonPath` 打开运行时，但所有 host 调用方传入的第 6 个入口参数仍是字面量 `'stub'` | 补丁：`native/engine/app_compat/apply.py:398-426`；调用方：`native/test/skyline-appservice-ops.test.js:101`、`skyline-page-smoke.test.js`（9 处）、`skyline-aot-smoke.test.js:89`、`skyline-appservice-smoke.test.js:105`、`tools/l5-baseline.js:129`；实测 `test:appservice` 报 `Failed to load dynamic library 'stub'` | 高 |
| R2 | 上述失败没有产生任何信号：CI 只恢复 release 引擎，依赖 debug 引擎 / JIT kernel 的测试全部 skip，且 CI 未设置 `SKYLINE_REQUIRE_ENGINE` | `.github/workflows/release.yml:42-58` | 高 |
| R3 | 生成的 `devtools.dart` 键盘通道仍执行 `DynamicLibrary.open('libskyline.so')`，而 4c0c397 起 staging 不再提供该文件。目前只有独立 host 的 `--key-script` 会走到这里，属于潜在故障 | `apply.py:322` | 中 |

### 3.2 构建流水线与产物一致性

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| B1 | 步骤之间没有输入指纹，`--check` 只判断文件存在或源码已打补丁。`runtime:embed --no-build` 之后引擎仍是旧的，却能通过 `runtime:embed:check` | `build-engine.sh:53-57`；`patch-skyline-runtime.sh:75-91` | 高 |
| B2 | `kernel:skyline:check` 先执行 `apply.py` **写入**源码树，再校验已有的 kernel，“刚修好的源码 + 过期 kernel”会被报告为 ok；约 17 处编辑中只有 7 处被 grep 校验 | `build-skyline-kernel.sh:64-91` | 高 |
| B3 | `apply.py` 以字符串替换修改恢复出的 Dart 源码：部分 import 插入不校验锚点（`.replace` 未命中时钩子照样插入，产出无法编译的代码），首组 yoga 编辑锚点缺失时静默跳过。`dart_ui_compat` 已在 3e9bb16 迁移为 unified diff，这里仍是旧做法 | `apply.py:39-68,272-276,306-310` | 中 |
| B4 | `patch-skyline-runtime.sh` 用文本锚点改写引擎 checkout 中受版本控制的 `BUILD.gn` 与 `embedder_exports.lst`，并靠删除 `.so` 触发重链接；`engine:fetch` 会还原这些修改，需要人工记得重跑 | `patch-skyline-runtime.sh:126-291` | 中 |
| B5 | `ui:patch` 在 debug 构建目录不存在时以 0 退出、只打印警告，fresh clone 上“成功”了却没有产出 patched SDK；而且固定使用 debug 目录，release 引擎需要另外记得重建 | `patch-dart-ui.sh:112-115`；`env.sh:20` | 中 |
| B6 | `build-aot.sh` 在 release 引擎缺失时静默回退到下载的 `gen_snapshot`，而脚本注释自己写明这样会产出不兼容的快照 | `build-aot.sh:74-84` | 中 |
| B7 | `engine:check` 用 `;` 连接两个检查，前一个失败会被吞掉 | `package.json:58` | 低 |
| B8 | 路径常量与兄弟目录（`../wechat-web-devtools-linux`、`../skyline-client-server`）在 8 处以上的脚本、测试和工具中各写一份；CMake 安装 rpath 固定指向 debug 引擎 | `env.sh:17,20`；`stage-devtools-addon.sh:57`；`test/lib/env.js:30-44`；`tools/record-skyline-ops.js:78`；`native/CMakeLists.txt:98,240` | 低 |
| B9 | DevTools 与 wxlib 版本没有固定（只出现在注释和 spec 元数据里），而内置组件样式会随 wxlib 版本变化 | `tools/extract-wxlib-style.js:34,54` | 低 |

### 3.3 原生运行时（`native/runtime/`）

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| N1 | 没有分层：每个 `.cc` 通过 `skyline_runtime_module.hh` 引入全部内部头；`skyline_runtime_appservice.hh`（601 行）同时定义 `AppStyle`（108 个字段）、`AppNode`（约 100 个字段）、`OpPage`（51 个字段）、34 个全局量与 sink 类型 | `internal/skyline_runtime_module.hh:3-18`；`internal/skyline_runtime_appservice.hh:230-370,550-581` | 高 |
| N2 | 模块之间有环：layout → image（`LoadImageForNode`）→ update（`RebuildAppServicePage`）→ layout；widget 中直接调用 `LayoutNode` / `MeasureNode` 各 4 次 | `appservice_layout.cc:14,172`；`appservice_image.cc:320,336` | 中 |
| N3 | 约 50 个可变全局量只由 2 把互斥锁保护，线程归属靠约定。`g_page`、`g_ui_task_runner` 在 UI 线程写入、在 addon / JS 线程读取，存在数据竞争；`g_texture_dispose` 不是原子量 | `appservice_model.cc:43,98,248`；`appservice_bridge.cc:387,517-529`；`appservice_image.cc:536,687` | 高 |
| N4 | `AppNode` 同时承载 DOM、样式、布局和 31 个 Dart 句柄，并内嵌 3 份完整的 `AppStyle`（`style` / `animation_override` / `previous_style`） | `internal/skyline_runtime_appservice.hh:246,260,338,367` | 中 |
| N5 | 巨型函数：`BuildWidget` 1062 行，`LayoutNode` 677 行，`ApplyOperation` 556 行（约 70 种 op），`ApplyDeclaration` 498 行（111 个 `key ==` 分支），`ApplyTargetedUpdates` 398 行，`MeasureNode` 327 行；6 个文件超过 1000 行 | `appservice_widget.cc:658-1719`；`appservice_layout.cc:506-1182`；`appservice_ops.cc:780-1335`；`appservice_css.cc:1635-2132`；`appservice_update.cc:238-635` | 高 |
| N6 | 录制回放 `ApplyOpsPage` 与实时回放 `ApplyLiveOpsBatch` 是两份平行实现（93 行逐字相同）。像“attach 延后到批末物化”这样的修复，需要在 3 个循环里各改一次 | `appservice_page.cc:318-598,608-791` | 高 |
| N7 | `FindCallback` + `reinterpret_cast` 有 210 个调用点，散落在各模块；同一组 13 个回调在 4 处重复查找。`FindCallback` 是线性扫描，每构建一个节点都会调用 | `appservice_page.cc:25-51`；`appservice_update.cc:678-707`；`appservice_builder.cc:83-118,281-310`；`skyline_runtime_stub.cc:155-165` | 中 |
| N8 | FFI 签名与哈希靠手工维护：143 个哈希常量和 116 个函数 typedef 只覆盖约三分之一的官方回调，也没有测试与 `spec/kernel-symbols.json` 比对；Float / Double 宽度写错已经多次造成静默错值 | `internal/skyline_runtime_ffi.hh`（763 行），`:341-349` | 中 |
| N9 | 魔法数：CSS `position` 被映射为 0–4 后在 6 个文件中裸比较；`GridViewType` 与 `GridBuilderType` 的枚举顺序相反；合成地址 / ID 基址 `0x1100`、`0x5000`、`0x30000000`、`0x40000000`、`0x80000000` 分散在各处 | `appservice_css.cc:1658-1661`；`appservice_widget.cc:884,1207`；`appservice_model.cc:251`；`appservice_image.cc:378` | 中 |
| N10 | 属性名别名（camelCase / kebab-case / 历史拼写）在 node、image、widget、layout 中各写一份，`CamelToKebab` 只在 ops 中使用 | `appservice_node.cc:120-125,602-608`；`appservice_widget.cc:286-294`；`appservice_ops.cc:159` | 低 |
| N11 | 演示代码与生产代码混编：`appservice_demo.cc`（1337 行）总是编入引擎，生产路径还依赖其中的 `CreateTextWidget`、`ReadDescriptor`、`ParseColorString`、`MemberNumber` | `appservice_demo.cc:556`；`appservice_widget.cc:203,703`；`appservice_page.cc:334,619` | 中 |
| N12 | 日志：252 处裸 `fprintf(stderr)`，不少位于热路径且不受调试开关控制（每次图片请求、每个按键、每批 live op 都会打印）；`ReportDartError` 一律带着误导性的前缀 `demo page:`；缺失回调和非法 op 被静默忽略 | `appservice_bridge.cc:17-47,513`；`appservice_image.cc:315,488`；`appservice_support.cc:6-38` | 中 |
| N13 | 无法单元测试。CSS、选择器和布局只在写布局缓冲时接触 Dart（`appservice_layout.cc:1189-1260`），文本测量是纯 C++ 字形表（`appservice_node.cc:240`）。但受 N1、N3、N4 牵连，这些逻辑只能在完整引擎中编译和运行 | `native/CMakeLists.txt` 只编译 `[common]` 部分 | 高 |

### 3.4 Addon / 宿主 / 运行时边界

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| A1 | 没有公共 API 头文件：runtime 导出 85 个 `extern "C"` 函数，addon 手写其中 34 个的声明，两个 host 再各写一组 weak extern，签名漂移只能在链接期或运行时暴露 | `native/src/main.cc:21-95`；`native/host/host_engine.cc:113-115`；`native/host/main.cc:32-42` | 高 |
| A2 | `Engine` 抽象（6 个操作）已经被绕开：滚动、swiper、焦点、sink、无障碍、快照、导航等大部分能力都直连 runtime | `native/include/engine.hh` | 中 |
| A3 | TSFN 生命周期不对称：HTTP TSFN 在 `CleanupCallbacks` 中从未释放；`OnWindowRasterized` 先释放旧 TSFN 再替换引擎回调，并发的 present 可能调用已释放的 TSFN；替换失败时新 TSFN 泄漏 | `main.cc:156,196-230,699-708` | 高 |
| A4 | `callbacks` 表通过 `napi_ref` 永久持有所有 `setXxxCallback` 传入的函数，但从未读取 | `main.cc:254-263` | 低 |
| A5 | 41 个 N-API 回调各自重复 `napi_get_cb_info` 解析样板；137 次 `napi_*` 调用中只有 32 次检查返回状态 | `main.cc` 全文 | 中 |
| A6 | 独立 host 与 `host_engine.cc` 重复实现 vsync / baton、BGRA→RGBA 拷贝、语义树日志，以及引擎 / AOT / ICU 资源发现（两处候选顺序还不一致） | `host/main.cc:94-235,538-640` 对比 `host_engine.cc:36-180,479-606` | 中 |
| A7 | 环境变量分散读取：native 代码读取 16 个 `SKYLINE_*`，其中 `SKYLINE_ASSETS`、`SKYLINE_DEVTOOLS`、`SKYLINE_DEVTOOLS_DEMO_OPS`、`SKYLINE_DUMP_OPS`、`SKYLINE_LAYOUT_DEBUG_ID`、`SKYLINE_POINTER_VIEW_ID`、`SKYLINE_SHARED_MEMORY_HANDLE` 没有出现在任何文档里；测试与工具另外读取几十个变量（如 `SKYLINE_HOST_RESULT`、`SKYLINE_L5_WINDOWS_BASELINE`），同样没有统一参考 | `grep getenv native/` | 中 |
| A8 | addon 日志每次调用都打开、追加、关闭 `/tmp/skyline-addon.log`，没有大小上限；模块初始化失败只返回 `nullptr`，调用方只能看到通用的加载错误 | `main.cc:1487-1512,1588-1606` | 低 |
| A9 | shared-memory 用 node-addon-api，skyline.node 用裸 N-API，两种风格并存；shared-memory 的 `Logger` 模板忽略全部参数，格式占位符原样输出 | `native/shared-memory/src/logger.hh:22-45` | 低 |
| A10 | `/dev/shm/skyline_<key>.dat` 路径、8 字节帧头、60 Hz 周期等协议常量在多处重复定义 | `host/frame_writer.cc:18,52`；`shared-memory/src/memory/manager.cc:138`；`host_engine.cc:35-36` | 低 |

### 3.5 Bootstrap JS（`native/src/bootstrap/`）

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| J1 | 14 个文件只是同一个 IIFE 的文本切片，不能单独解析（`00-shell.js` 以 `const definitions = [` 结尾，`10-shell-prototype.js` 接着写这个数组）；状态靠 16 个顶层 `let` 和 29 个 `Map` / `Set` 在闭包中共享 | `00-shell.js:505`；`10-shell-prototype.js:1`；`120-tail.js:36-46` | 高 |
| J2 | 31 个空 `catch {}`（`00-shell.js` 中有 12 个），例如 `native.applyPageOps` 失败会静默丢弃整批页面操作 | `00-shell.js:450-456` | 高 |
| J3 | 5 个常驻或长时间运行的轮询，另有约 23 处 `setTimeout`：8 ms 帧补发泵、16 ms flush 泵、100 ms worklet 注入（持续 20 s）、250 ms console 过滤注入（永不停止），以及一个以字符串形式注入 iframe 的 16 ms 滚动监视（已禁用但仍在代码中） | `00-shell.js:470-485`；`120-tail.js:4,22-28`；`115-console-filter.js:86`；`90-scroll.js:11,79-104` | 中 |
| J4 | 对 SDK 内部细节的依赖硬编码且分散：消息码 `'SAC6'`、iframe 名 `app_sub_render`、`__wxElement` / `_$backendElement`、worklet 的 `_closure`；还有一段以深度 6、最多 500 个对象遍历闭包来收集 mutable 输入的鸭子类型逻辑 | `00-shell.js:38,55-70`；`50-worklet-bridge.js:13-58` | 中 |
| J5 | API 层规范手写了三份：`reference/skyline-node.js` 的 31 个方法定义、bootstrap 中同序同名的实现、C++ 中的 `DEBUG_INFO`；契约测试一次只针对其中一个目标，漂移无法发现 | `reference/skyline-node.js:46-95`；`10-shell-prototype.js:1-215`；`main.cc:1463-1469` | 中 |
| J6 | 没有 lint、类型检查或分片语法检查，语法错误要到 addon 加载时才暴露 | `test/source-manifest.test.js` 只校验清单与 IIFE 边界 | 低 |

### 3.6 测试体系

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| T1 | 没有不依赖引擎的 CSS / 选择器 / 布局单元测试；所有布局覆盖都需要 host + 引擎 + kernel，每个用例都启动一次完整引擎 | `native/CMakeLists.txt` 中没有 `add_test` | 高 |
| T2 | `skyline-appservice-ops.test.js` 单文件 5284 行、103 个用例：42 个用例内联 ops；294 处硬编码坐标的 `pixelAt(x, y)`，159 处日志正则匹配（含 `[ops-debug]` 文本）。布局稍有变化就会让大量断言失效，而且失败难以诊断 | `native/test/skyline-appservice-ops.test.js:60-145` | 中 |
| T3 | host 启动样板重复：`prerequisiteSkip` 在 12 个测试文件中各定义一次，`parseResult` 有 5 份，`runHost` 与 `tools/l5-baseline.js:100-146` 几乎相同 | — | 中 |
| T4 | L5 比较对缺失的夹具直接 `continue`，不检查“夹具文件 ↔ FIXTURES 列表 ↔ 基线”三者是否一致：60 个夹具中只有 40 个在列表里，Linux 基线只有 28 条；基线不记录引擎 / AOT 哈希，引擎重建后基线也不会失效 | `native/test/l5-pixel-compare.test.js:43-47,74-76`；`tools/l5-baseline.js:38-95` | 高 |
| T5 | fresh clone 上约 76% 的用例被跳过，但结果显示为绿；前置条件的处理方式不统一（skip、exit 77、`SKYLINE_REQUIRE_ENGINE` 并存） | — | 中 |
| T6 | 契约测试默认针对 JS 参考实现，CI 从未对真实的 `skyline.node` 运行 `test:native` | `test/contract.test.js:26-29` | 中 |

### 3.7 工具、基线与 CI

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| C1 | 发布包由“新编译的 `skyline.node`”和“手工上传、可变的 `build-deps`（含嵌入式运行时的引擎 + AOT）”拼成。CI 从不重新嵌入运行时，也不重建 kernel / AOT，所以 `native/runtime` 与 `apply.py` 的改动不会进入发布包，而且没有任何校验能发现这一点 | `release.yml:42-58` | 高 |
| C2 | CI 自己重写了打包步骤，绕过了 `stage-devtools-addon.sh` 中的符号检查、RUNPATH 设置、AOT 与 kernel 互斥等校验 | `release.yml:60-87` 对比 `stage-devtools-addon.sh:116-198` | 中 |
| C3 | `build-deps` 没有校验和，也没有记录来源提交；actions 只按主版本 tag 固定 | `release.yml:44-45,82` | 中 |
| C4 | Wine 侧的两个 Electron 宿主（`l5-win-app`、`win-oracle-app`）复制了同一套启动和回调注册代码；`l5-windows.js` 的 `ensureShim` 与 `test/lib/win-shim.js` 逐字相同；像素比较循环共有 3 份 | `tools/win-oracle-app/renderer.js:30-45`；`tools/l5-windows.js:112-136` | 中 |
| C5 | `spec/l5-baseline.win32.json` 有 860 KB、72k 行，以 zlib + base64 文本内嵌帧，被 18 个提交改动过，单次最多 +10,860 行，diff 无法审阅 | `git log -- spec/l5-baseline.win32.json` | 低 |
| C6 | `npm run gaps` 还没有进入 CI（D5 未完成） | `docs/phase-3-tasks.md:274` | 低 |

### 3.8 文档与开发体验

| # | 问题 | 证据 | 严重度 |
|---|---|---|---|
| D1 | README 是 415 行的进度流水账；测试计数（“198/174/24”“ops 75”）、“25 个夹具”、“引擎为 stub”、“StubEngine 不含渲染”、“同时构建 stub libskyline.so”等描述都已过期 | `README.md:77,132,191-196,241,377` | 中 |
| D2 | `docs/phase-3-tasks.md` 被声明为进度的唯一来源，但同一文件中两处测试基线数字互相矛盾，D 段 12 项也没有计入总数 | `docs/phase-3-tasks.md:412,464-472` | 中 |
| D3 | 没有架构文档；架构描述分散在 README、`native-roadmap.md`、`phase-3-dart-ui-compat.md`、`feasibility.md` 中，并各自漂移 | — | 中 |
| D4 | 没有“改了 X 要重建 Y”的说明，也没有环境变量参考；`AGENT.MD` 只有一句话，没有给出逐像素比对的入口命令 | `AGENT.MD` | 中 |
| D5 | `.vscode/settings.json` 写死了个人机器上的绝对路径 | `.vscode/settings.json:2` | 低 |

### 3.9 顺带发现的缺陷（建议独立修复，不混入重构）

1. 嵌套的 `@media` / `@supports` 递归调用 `ParseStyleSheet(body)` 时丢失了
   `style_scope` / `context_id`，组件样式隔离在媒体查询内部失效
   （`appservice_selector.cc:374,376`）。
2. 每个路由的 `SkylinePageConfig` 是函数内的 `static`，新的 attach 会覆盖仍被
   旧路由持有的指针（`appservice_page.cc:158-159`）。
3. `ApplyOpsPage` 每次都 `new OpPage()`，但不释放旧对象（`appservice_page.cc:367`，
   测试路径上的泄漏）。
4. `FfiTable::Register` 超过 4096 项后静默丢弃（`skyline_runtime_stub.cc:101-107`）。
5. A3 中的 TSFN 释放顺序与泄漏问题。

### 3.10 值得保留的做法

- `PostToUiIfReady` + `DemoPageApiScope` 这种统一的切线程 / isolate 作用域模式；
  sink 原子指针，以及与活动树隔离的 `g_rect_snapshot` 快照；
- `dart_ui_compat` 已改为 unified diff，并用 `git apply --reverse --check` 校验；
- 9 个引擎脚本统一使用 `set -euo pipefail` 并共享 `env.sh`；
- `REVISION`、`ARTIFACTS.json`、`spec/kernel-symbols.json` 中的引擎 / Dart /
  Skyline 版本彼此一致；
- 在 Wine 中运行官方引擎作为 L5 基准的整体思路，以及 bootstrap 已有的 VM 级单测
  （`test/bootstrap-scroll.test.js`）。

## 4. 重构原则

1. **行为不变优先**：重构提交不改变像素输出；行为修复单独提交，并附带夹具。
2. **先建安全网，再拆结构**：P0–P2 完成之前不动运行时结构。
3. **绞杀者模式，小步提交**：新结构与旧代码并存，逐个调用点迁移；每个 PR 都可以
   单独回滚，不使用长期分支。
4. **每一步都可验证**：每个 PR 必须通过 engine-free 测试、布局 golden 逐字节一致、
   `test:ops`，以及 `test:l5`（与 Windows 基线的比较结果不变）。
5. **单一事实来源**：符号与签名、环境变量、路径、版本、API 方法表都只定义一次，
   其余位置通过生成或引用获得。
6. **保持零 npm 依赖、测试不联网**。

## 5. 目标架构

### 5.1 目录结构

```
native/
  include/skyline/
    runtime_api.h            # runtime 对外 C ABI 的唯一声明处，标注线程归属
    frame_protocol.h         # 共享内存路径、帧头、像素格式等协议常量
  runtime/
    core/                    # 纯 C++17，不依赖 Dart/Flutter/Skia，CMake 中可单独编译
      dom/                   # DomNode 树、属性规范化（别名表）、ops 解析
      css/                   # 声明解析、属性处理表、颜色/长度/transform/gradient
      selector/              # 样式表解析、选择器匹配、级联与作用域
      layout/                # measure、flex、inline、grid、absolute/fixed/sticky
      text/                  # TextMeasurer 接口 + 字形表实现
    materialize/             # Dart 侧：RuntimeCallbacks、NodeHandles、各 tag 的 WidgetBuilder、定向更新
    session/                 # OpsSession：批处理 / attach / 路由栈（录制与实时共用）
    bridge/                  # runtime_api.h 的实现、sink、UI 线程切换、几何快照
    ffi/                     # 由 spec/kernel-symbols.json 生成的签名与哈希表
    demo/                    # SKYLINE_STUB_PAGE 演示页（受构建选项控制）
    tools/skyline_layout_cli.cc   # ops JSON → 布局 JSON，供 CI 中的 golden 校验使用
  src/                       # skyline.node：表驱动的 N-API 绑定、ThreadsafeSink RAII
    bootstrap/               # 可独立解析的 install 模块
  host/
    embedder/                # 两个宿主共用：vsync、present、语义树、资源发现
  shared-memory/
test/
  lib/host.js                # 唯一的 host 启动 / 前置条件 / 结果解析 / 取像素入口
  golden/layout/*.json       # 每个夹具的结构化布局快照
native/test/ops/*.test.js    # 按领域拆分的 ops 测试
tools/lib/                   # L5 harness、Wine/Electron 宿主公共部分
```

### 5.2 运行时分层与依赖方向

```
  bridge（C ABI、线程切换）          session（OpsSession：批处理 / attach / 路由栈）
        │                                   │
        ▼                                   ▼
  materialize（Dart 句柄、WidgetBuilder、定向更新）────► ffi（生成）
        │
        ▼
  core：dom ─► css ─► selector ─► layout ─► text      （只允许沿箭头方向依赖）
```

规则：`core` 不得包含 Dart、Flutter 或 Skia 头文件，也不访问全局量（视口尺寸、rpx
基准、主题等通过 `LayoutContext` 显式传入）；图片固有尺寸等外部信息通过接口回调
提供，从而解开 layout → image → update → layout 的环。

### 5.3 关键接口草图

```cpp
// native/include/skyline/runtime_api.h：runtime、addon、host 共用的唯一声明
// 线程约定：[any] 任意线程；[ui] 仅 UI task runner；[any→ui] 内部切到 UI 线程
SKYLINE_API void SkylineApplyLiveOps(const char* json, size_t len);   // [any→ui]
SKYLINE_API int  SkylineGetNodeRect(int64_t node_id, float out[5]);   // [any] 读快照
SKYLINE_API void SkylineSetScrollEventSink(SkylineEventSink sink);    // [any]
SKYLINE_API const char* SkylineRuntimeBuildId(void);                  // [any] 运行时源码指纹
```

```cpp
// materialize/runtime_callbacks.h：holder 初始化时一次性解析，取代 210 处 FindCallback
struct RuntimeCallbacks {
  NewYGNodeFn new_yg_node;
  NewYGConnectorFn new_yg_connector;
  NewTextSpanFn new_text_span;
  // ……其余字段由 ffi/ 生成
  static RuntimeCallbacks Resolve(const FfiTable& table,
                                  std::vector<std::string>* missing);
};
```

```cpp
// core/dom/dom_node.h：不含任何 Dart 句柄
struct DomNode {
  NodeId id;
  std::string tag, name;
  AttributeMap attrs;            // 入口处统一规范化为 kebab-case
  std::vector<NodeId> children;
  ComputedStyle style;           // 动画覆盖层另存为稀疏的 StyleOverlay
  LayoutBox layout;
};
// materialize/node_handles.h：按 NodeId 索引的 Dart 持久句柄旁路表，RAII 释放
```

```cpp
// session/ops_session.h：录制夹具 = 若干批次的顺序回放，与实时路径共用同一实现
class OpsSession {
 public:
  void ApplyBatch(const rapidjson::Value& ops, BatchOrigin origin);  // attach 延后到批末物化
};
```

## 6. 分阶段实施计划

### P0 止血（1–2 人日）

任务：

1. 修复 R1：集中生成 host 的入口参数（P2 之前先放一个小 helper），第 6 个参数传入
   真实可 `dlopen`、并导出 Skyline ABI 的库路径（host 场景下是对应模式的
   `libflutter_engine.so`）。同时在补丁中加入兜底：路径不存在时回退到
   `skylineDylibNameSo`，避免再次出现整片失败。
2. 修复 R3：键盘通道复用 `openSkylineDylib` 得到的库句柄，不再按名字打开
   `libskyline.so`。
3. L5 测试改为集合校验：每个夹具要么出现在 `tools/l5-baseline.js` 的 FIXTURES 中，
   要么在显式的排除列表里（注明原因）；FIXTURES 与两份基线的键必须一致（允许
   `compare: false`），任何缺失都直接失败。
4. 在本机跑通 `npm test`、`test:ops`、`test:l5`，把真实计数写进 PR 描述；README
   删除手写的计数，改为指向命令或 CI。
5. 修正 B5、B6、B7：`ui:patch` 在没有构建目录时失败；`build-aot.sh` 不再静默回退，
   而是失败并提示；`engine:check` 改用 `&&`。

验收：本机全量测试通过（失败项都有登记的已知原因）；60 个夹具在 L5 中都有明确的
状态（参与比较或已豁免）。

**状态（2026-11-27）：已完成。**

- R1：`apply.py` 的 dylib 补丁只在 `skylineAddonPath` 是已存在的文件时按路径打开，
  否则回退到 `libskyline.so`，并能把 c110f03 版本的补丁原地升级。host 测试和
  `tools/l5-baseline.js` 统一通过 `test/lib/host.js` 的 `devtoolsEntrypointArgs()`
  传入真实库路径（嵌入式运行时传对应模式的 `libflutter_engine.so`，独立 stub 测试传
  `native/build/libskyline.so`），走与 DevTools 相同的按路径打开逻辑。
- R3：键盘通道改用 `SkylineApplication.instance?.dylib`；`kernel:skyline:check`
  对两处补丁都做校验。
- L5：`tools/l5-baseline.js` 新增 `EXCLUDED_FIXTURES`（20 个夹具，逐项注明原因）
  和 `fixtureSetProblems()`；`test:l5` 新增不依赖引擎的集合校验，缺失夹具或基线
  条目改为失败（`tools/l5-windows.js` 的比较模式同样如此）。Linux 基线补齐了 12 个
  缺失条目，原有 28 条哈希不变。
- B5、B6、B7 已修正；README 删除了手写的测试计数，L5 段落改为引用夹具列表和
  `npm run gaps`。
- 本机验证（两种模式重新嵌入运行时，重建 kernel 与 AOT 之后）：`npm test` 共 255 个
  用例，231 pass、0 fail、24 skip（均为契约用例在 JS 参考实现上没有引擎）；其中
  `test:ops` 103/103；`test:l5` 3/3；`test:native` 53 pass、10 skip（官方进程崩溃的
  用例）；AOT 冒烟 3/3；`runtime:embed:check`（两种模式）、`kernel:skyline:check`、
  `ui:patch:check`、`engine:check` 均通过。
- 不在 P0 范围：CI 仍不运行依赖引擎的测试（R2，留给 P1 第 8 项）。

### P1 构建流水线可追溯（4–6 人日）

任务：

1. **构建清单**：每个步骤（`ui:patch`、`engine:build`、`runtime:embed`、
   `kernel:skyline`、`devtools:aot`、`native:build*`）在产物目录写入
   `build-manifest.json`，记录输入文件集合的 sha256（补丁、`dart_ui_compat`、
   `sources.txt` 所列运行时源码、`apply.py`、恢复出的源码树）、`REVISION`、构建
   模式和产物哈希。
2. **运行时指纹写入引擎**：`runtime:embed` 生成导出函数 `SkylineRuntimeBuildId()`
   （返回运行时源码哈希）。addon 加载时与自己编译时记录的哈希比较并告警；host
   测试和 `devtools:stage` 遇到不一致直接失败，并给出需要执行的命令。
3. `tools/build-status.js`（`npm run build:status`）：比较当前源码与各个清单，输出
   “哪些步骤过期”以及“按顺序该运行的命令”；可选提供
   `npm run build -- <devtools|tests> --mode <debug|release>`，按顺序执行过期步骤。
4. 所有 `--check` 改为零写入：`build-skyline-kernel.sh` 在 check 模式下不再运行
   `apply.py`，而是对比已打补丁的源码树与 kernel 的清单。
5. `apply.py` 改造为 unified diff（与 3e9bb16 对 `dart_ui_compat` 的做法一致）：在
   临时 git 仓库中对确定性恢复出的源码树应用 `native/engine/patches/app-compat/*.patch`，
   `--check` 用 `git apply --reverse --check`。
6. 路径与版本的单一来源：`native/engine/env.sh` 和 `test/lib/env.js` 从同一份配置
   读取兄弟目录；`REVISION` 增加 `DEVTOOLS_VERSION` 与 `WXLIB_VERSION`，工具从这里
   读取。
7. CMake：安装 rpath 按引擎模式推导；独立的 `libskyline.so` 目标改为构建选项
   （P0 之后确认是否还有测试依赖，没有就删除）。
8. CI：`build-deps` 按“引擎输入指纹”命名，附带 sha256 与来源提交；CI 校验下载的
   引擎中 `SkylineRuntimeBuildId()` 与仓库运行时源码一致，不一致则失败并提示重建
   `build-deps`；打包改为调用 `stage-devtools-addon.sh --output <dir>`；增加各个
   `*:check` 与 `test:native`。

验收：故意修改一个运行时 `.cc` 而不重新嵌入引擎，`npm run test:ops`、
`devtools:stage` 和 CI 都会明确失败，并提示正确的命令；干净构建之后
`build:status` 报告所有步骤都是最新的。

**状态（2026-11-27）：已完成，个别条目有调整。**

- 指纹与 stamp：`tools/build-status.js` 计算三组输入指纹（运行时源码、`dart:ui`
  补丁、`app_compat` 补丁）。`patch-dart-ui.sh`、`build-engine.sh`、
  `build-skyline-kernel.sh`、`build-aot.sh` 在产物旁写 `skyline-build*.json`，
  记录本次消费的指纹与源码提交。
- 运行时指纹：`build-engine.sh` 在 `ninja` 之前，按引擎 checkout 中实际编译的运行时
  副本生成 `skyline_runtime_build_id.hh`；`libflutter_engine.so` 导出
  `SkylineRuntimeBuildId()`，启动日志 `embedded Dart API ready (build …)` 也会打印
  该指纹。工具直接从二进制中读取标记串，不依赖 stamp。与计划的差异：addon 加载时
  不做比较，改由测试、staging 与 CI 拦截。
- `npm run build:status` 按流水线顺序列出过期步骤与重建命令。host 测试（每个文件
  一个新鲜度用例，`SKYLINE_ALLOW_STALE_BUILD=1` 可改为 skip）、`l5:baseline`、
  `devtools:stage`、`pack-build-deps.sh` 与 CI 都拒绝过期产物。未提供
  `npm run build` 自动编排，按提示的命令手动执行。
- `--check` 零写入：`kernel:skyline:check` 只做 `git apply --reverse --check` 与
  stamp 比较。
- `apply.py` 改为 6 个按文件划分的 unified diff（`native/engine/app_compat/*.patch`，
  补丁头保留原有说明），用 `git apply` 应用，无法应用时提示 `--refresh-sources`。
  补丁由原脚本在纯净恢复树上的输出生成，现有工作树逐字节一致，kernel 不变。
- 顺带修复：3e9bb16 删除了复制 `dart_ui_compat/*.dart` 的步骤，修改这三个文件不会
  进入引擎；`ui:patch` 现在重新安装并按内容校验。
- 路径与版本：`test/lib/env.js` 新增 `devtoolsDir()`（读取与 staging 相同的
  `SKYLINE_DEVTOOLS_DIR`）、`windowsResourceCandidates()` 与 `revisionValue()`，
  录制器、契约测试与 `wxlib:style` 改用它们；`REVISION` 固定
  `DEVTOOLS_VERSION=2.02.2609250` 与 `WXLIB_VERSION=2.25.3`，`wxlib:style` 默认使用
  后者。shell 脚本仍各自读取 `SKYLINE_DEVTOOLS_DIR`。
- CMake：删除固定指向 debug 引擎、且因为没有 install 规则而从未生效的
  `INSTALL_RPATH`。独立的 `libskyline.so` 仍被 `test:appservice` 使用，因此保留。
- CI：下载 `build-deps` 后校验 sha256，并用 `build-status --check engine-release,aot`
  确认引擎与 AOT 对应当前源码；新增 `test:native`；打包改为
  `stage-devtools-addon.sh --output`（归档布局不变）。新增
  `native/engine/pack-build-deps.sh`，生成带 stamp 与 `BUILD-DEPS.json` 的 deps 包。
- **需要人工处理**：现有 `build-deps` release 没有 `.sha256` 与 stamp，合入 `master`
  之前需要用 `pack-build-deps.sh` 重新发布，否则 CI 会在校验步骤失败。actions 仍按
  主版本 tag 固定（C3 未做）。
- 验证：一次完整重建后 `build:status` 全部为 ok；故意修改
  `native/runtime/appservice_css.cc` 后，`build:status`、`test:ops` 的新鲜度用例与
  `stage-devtools-addon.sh --output` 都失败，并提示 `npm run runtime:embed`；修改
  `app_compat` 或 `dart:ui` 补丁时，对应的 SDK、引擎、kernel 与 AOT 都被标记为过期。
  最终状态下 `npm test` 共 260 个用例（新增 5 个新鲜度用例），236 pass、0 fail、
  24 skip；`test:native` 53 pass、10 skip。

### P2 测试基础设施（5–8 人日）

任务：

1. `test/lib/host.js`：统一 host 启动（参数、共享内存 key、超时、环境变量）、前置
   条件检查（统一返回 skip 原因；设置 `SKYLINE_REQUIRE_ENGINE=1` 时转为失败）、结果
   解析和取像素 / 颜色统计。迁移 13 个 native 测试文件和 `tools/l5-*.js`。
2. **结构化布局快照（golden）**：host 增加 `--dump-tree <file>`，输出每个节点的
   id、tag、类名、解析后的关键样式、绝对矩形和物化出的 widget 种类；为 60 个夹具
   生成 `test/golden/layout/*.json`，提供 `npm run golden:check` / `golden:update`。
   它是 P3–P5 的主要安全网：比像素便宜、可读，失败时能直接定位到具体节点和属性。
3. 拆分 `skyline-appservice-ops.test.js`：按领域拆为
   `native/test/ops/{css,layout,text,scroll,gesture,swiper,route,image,channel,worklet}.test.js`；
   内联 ops 抽成夹具；能用结构化结果断言的地方，替换硬编码坐标的 `pixelAt` 和日志
   正则（确实需要像素的断言保留）。拆分后 `node --test` 可以按文件并行执行。
4. FFI 哈希一致性测试：把 `skyline_runtime_ffi.hh` 中全部 `k*Hash` 与
   `spec/kernel-symbols.json` 比对（不依赖引擎，秒级完成）。
5. Bootstrap：对拼接后的脚本执行 `node --check`（P5 完成后改为逐片检查）。
6. L5 工具：抽出 `tools/lib/l5-harness.js`（渲染、比较、ignoreRects），供
   `l5-baseline.js`、`l5-windows.js`、`l5-pixel-compare.test.js` 共用；`--only`
   同时支持比较；失败时把差异 PNG 输出到 `test/results/`；基线记录引擎 / AOT 哈希。
7. 合并 Wine 宿主：`l5-win-app` 与 `win-oracle-app` 合为一个 Electron 宿主加场景
   插件；`l5-windows.js` 复用 `test/lib/win-shim.js` 和 `test/lib/env.js`。
8. 基线存储：Windows 帧改为每个夹具一个 PNG（`spec/l5/win32/<fixture>.png` 加一个
   小型索引 JSON），让 diff 可以审阅；至少也要按夹具拆分成独立文件。

验收：所有 host 测试只通过 `test/lib/host.js` 启动；60 个夹具都有 golden；ops 测试
并行后总时长下降；CI 输出 skip 数量，超出预期即失败。

### P3 边界与基础设施（5–8 人日）

任务：

1. 新增 `native/include/skyline/runtime_api.h`，收拢 runtime 全部对外 `extern "C"`
   声明并注明线程归属；runtime 定义处、addon 和两个 host 都包含它，删除手写声明和
   weak 声明（host 的可选符号在头文件中统一声明为 weak）。
2. 明确 `Engine` 的定位：改名为 `WindowBackend`，只负责窗口生命周期、输入和出帧；
   页面和查询类操作通过 `runtime_api.h` 访问，不再假装经过这层抽象。
3. Addon：
   - 表驱动注册（方法名 → 回调 + 参数规格），统一参数解码和错误抛出；
   - 用 `ThreadsafeSink` RAII 封装所有 TSFN（创建、unref 策略、释放），修复 A3：
     先替换引擎回调再释放旧 TSFN，在 cleanup 中释放 HTTP TSFN；
   - 删除无用的 `callbacks` 持有表，或让它真正发挥作用。
4. `native/host/embedder/`：抽出两个宿主共用的 vsync / baton、present 与
   BGRA→RGBA、语义树、资源发现和 `FlushPendingTasks` 循环；`frame_protocol.h`
   收拢共享内存协议常量，shared-memory 一并改用。
5. 配置：建立 `Config` 注册表（名称、类型、默认值、说明、所属组件），启动时读取一次；
   用 `npm run env:doc` 从注册表生成环境变量参考；CI 检查代码中出现的环境变量都已
   注册。
6. 日志：runtime、addon、shared-memory 共用分级日志宏
   `SKY_LOG(level, category, fmt, ...)`，默认只输出 warning 及以上；热路径日志降为
   debug；`ReportDartError` 带上真实上下文；文件 sink 设置大小上限；shared-memory 的
   `Logger` 改为真正的格式化输出。
7. 运行时全局状态：引入 `RuntimeState`，持有 page、UI runner、sink、视口等；
   `g_page`、`g_ui_task_runner` 的跨线程读取改为加锁拷贝或原子 `shared_ptr`；在
   UI 线程专属函数入口加 `SKY_DCHECK_UI_THREAD()`（仅 debug 构建生效）。改访问方式
   之前先加断言观察一段时间。

验收：`native/src` 与 `native/host` 中不再有手写的 runtime `extern "C"` 声明；TSFN
的创建与释放成对出现，并有测试覆盖（加载 → cleanup → 进程能正常退出）；可选：在
debug host 上用 ASan 运行 ops 测试，不出现新报告。

### P4 运行时内核重构（15–25 人日）

按以下顺序推进。每个子步骤都要求 golden 逐字节一致，`test:ops` 与 `test:l5` 的
结果不变。

1. **FFI 生成**：`tools/gen-ffi.js` 从 `spec/kernel-symbols.json` 生成签名 typedef
   和哈希表；`native/runtime/ffi/overrides.json` 记录与 JSON typedef 不符的实测签名
   （例如 `ScrollViewUpdateScrollTop`、`YogaTriggerIntersection*` 的 Float 参数）
   并附说明；同时生成 `RuntimeCallbacks`；`FindCallback` 改为哈希表查找。
2. **枚举与 ID**：引入 `enum class CssPosition`、`WidgetType`、`ScrollViewType`、
   `GridViewType` / `GridBuilderType`（分别映射，消除顺序相反的陷阱）、
   `RouteOpenType`；合成地址和 ID 基址集中到 `id_ranges.h`。
3. **属性规范化**：在 ops 入口统一执行 `CamelToKebab` 和别名表映射（`src` /
   `url` / `imageUrl` 等），下游只使用规范名；`AttrNumber` / `AttrBool` / `AttrText`
   收进 `AttributeMap`。
4. **隔离演示代码**：把 `CreateTextWidget`、`ReadDescriptor`、`ParseColorString`、
   `MemberNumber` 移到 materialize / css；`appservice_demo.cc` 移入 `runtime/demo/`，
   由构建选项控制，默认只编入 debug 引擎。如果 release AOT 冒烟测试依赖演示页，改用
   ops 夹具。
5. **统一回放会话**：用 `OpsSession::ApplyBatch` 合并 `ApplyOpsPage`、
   `ApplyLiveOpsBatch` 和 `updates` 回放循环；录制夹具等价于“首批 + `updates` 各批”
   的顺序回放；“attach 延后物化”只在一处实现。
6. **拆分数据模型**：`AppNode` 拆为 `DomNode`（纯数据）和 `NodeHandles`（Dart 持久
   句柄旁路表，RAII）；`animation_override` 改为稀疏覆盖层；`previous_style` 只在
   定向更新期间存在（或改为样式哈希 + 变更位集）。`OpPage` 拆为 `PageState`（路由与
   树）、`InputState`、`AnimationState`、`BuilderState`。
7. **抽取 `runtime/core` 静态库**：在 CMake 中作为不依赖引擎的独立目标，引入单头
   测试框架（如 doctest）编写 C++ 单测，覆盖：颜色 / 长度 / `calc` / `env` 解析，
   选择器特异性与作用域，级联顺序，`white-space` 归一化，flex（grow / shrink /
   basis / wrap / align / gap），absolute / fixed / sticky，`aspect-ratio`，
   `content-box`，inline-block 行盒，grid masonry 排布。同时提供
   `skyline_layout_cli`：读取 ops 夹具，输出与 P2 `--dump-tree` 同格式的 JSON，让
   CI 无需引擎就能校验 golden 中与物化无关的部分。3.9 第 1 条（嵌套 `@media` 作用域）
   在这一步作为独立提交修复，并附单测。
8. **拆分巨型函数**（在单测和 golden 的保护下进行）：
   - `ApplyDeclaration` → 属性处理表 `{name → handler}`，按 box / flex / text /
     visual / transform / animation 分文件；
   - `ApplyOperation` → 按 op 族分发（tree / attr / style / route / scroll /
     widget / automation）；
   - `BuildWidget` → 按 tag 注册的 `WidgetBuilder`（text、image、scroll-view、
     list/grid、swiper、picker、input、selection、texture、默认盒），
     `WrapVisualStyle`、Hero、Semantics 等作为装饰器链；
   - `LayoutNode` / `MeasureNode` → flex、inline 行盒、grid、absolute / fixed /
     sticky 子算法，加上公共的盒模型计算；
   - `ApplyTargetedUpdates` → 拆成“样式 diff → 变更集 → 更新器”三段。

   目标：单个函数不超过 200 行，单个文件不超过 1000 行。
9. **评估项**（不属于重构验收范围，单独立项）：
   - 基于 SkParagraph 的 `TextMeasurer`：现在布局用字形表近似测量，绘制却用
     SkParagraph，二者不一致是文本类差异（粗体宽度、换行、`white-space` 残差）的
     根源之一。core 抽取完成后，可以在嵌入式构建中注入真实的测量实现。
   - 以上游 Yoga 替换自研 flexbox 子集：从官方 FFI 命名（`YGNode*`、
     `YogaCalculateLayout`）推测，官方运行时基于 Yoga；近期不少提交（flex 默认
     shrink、交叉轴 margin、`aspect-ratio`、`content-box`）都是在补 Yoga 已有的语义，
     而 inline 流与文本仍需要自研。有了 `LayoutEngine` 接口和 golden 之后，可以低成本
     做 A/B 评估。

验收：所有子步骤保持 golden 逐字节一致；直接调用 `FindCallback` 的地方不超过 5 处；
runtime 中没有超过 1000 行的手写文件；core 单测在 CI 中运行。

### P5 Bootstrap JS 模块化（5–8 人日，可与 P4 并行）

任务：

1. 每个分片改为 `function installXxx(ctx) { ... }`，由 `ctx` 显式持有 `state`（原来
   的 16 个顶层 `let` 与 29 个集合）、`native`、`shell`、`scheduler`、`log`；各分片
   能单独通过 `node --check`。CMake 仍按 `parts.txt` 拼接，不引入打包器依赖。
   `__skylineDebug` 暴露 `ctx.state` 的只读视图。
2. 统一调度器 `FramePump`：帧补发、live flush、动画 tick、worklet mapper 统一由一个
   按需启停的泵驱动；一次性注入（worklet module、console 过滤）改为事件触发 + 超时
   停止；删除已禁用的字符串注入滚动监视。
3. 错误策略：31 个空 `catch {}` 替换为 `ctx.log.swallow(category, err)`（限频，转发
   到 addon 日志），对 SDK 仍保持不抛出；`applyPageOps` 失败必须可见。
4. SDK 适配层：新建 `sdk-compat` 模块，集中管理 SDK / glass-easel 的私有字段和常量
   （`SAC6`、iframe 名、`__wxElement`、`_$backendElement`、worklet `_closure`），
   并按 SDK 版本做能力探测，遇到未知版本时输出一次警告。
5. API 方法表单一来源：`reference/skyline-node.js` 与 bootstrap 共用一份
   `shell-definitions.js`（方法名、arity、错误消息、属性描述符）；CI 中的契约测试
   同时针对参考实现和真实 `skyline.node` 运行。
6. 可选：加 `// @ts-check` 和 JSDoc 类型，开发期用 `tsc --noEmit` 检查（需要接受
   一个 devDependency）。

验收：每个分片都能通过 `node --check`；`test/bootstrap-scroll.test.js` 全部通过；
空 `catch` 数量为 0；空闲页面上没有常驻定时器（用测试断言活动定时器数量）。

### P6 文档与开发体验（3–5 人日，随各阶段推进）

任务：

1. README 精简为四部分：项目是什么、当前能力与限制、快速开始、文档索引；进度叙述
   按日期移入 `docs/CHANGELOG.md`。
2. 新增 `docs/architecture.md`（组件、数据流、线程与通道、运行时分层，P3/P4 之后
   定稿）和 `docs/dev-workflow.md`（附录 A 的重建矩阵、`build:status` 用法、生成的
   环境变量参考、CDP 调试方法、L5 逐像素比对流程）。
3. `docs/phase-3-tasks.md` 冻结为历史记录；后续任务用 issue 或 `docs/roadmap.md`
   跟踪；测试计数只取自 CI 输出，不再手写。
4. 扩充 `AGENT.MD`：构建矩阵、提交前必跑的命令、逐像素比对的入口
   （`npm run l5:windows -- --only <fixture>` 及差异图所在位置）。
5. 删除或修正过期描述（libskyline.so、StubEngine、测试计数、夹具数）；
   `.vscode/settings.json` 改用 `${workspaceFolder}`。
6. D5：`npm run gaps -- --json` 进入 CI，并提交 `docs/conformance-gaps.md` 作为基线。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 重构引入像素回归 | golden 与 L5 双重把关；重构提交和行为修复严格分开；golden 的任何变化都必须在 PR 中说明原因 |
| 每次改运行时都要在两种模式下重链接引擎，迭代慢 | 优先完成 core 抽取（P4.7），让大部分 CSS / 布局改动能在秒级单测中验证；构建清单避免多余的重建 |
| 与持续进行的功能修复冲突 | 绞杀者模式小步合并；按模块排期（例如某段时间只动 `css/`），并在提交说明中标注 |
| 恢复出的源码随 DevTools 版本变化，导致补丁失效 | P1 固定 DevTools / wxlib 版本；补丁改为 diff 之后，冲突会显式暴露 |
| 重建 `build-deps` 依赖本机的大体量构建 | 指纹校验让 CI 明确提示何时需要重建；记录重建步骤与来源提交 |
| 过度设计 | 每个阶段都有可量化的验收标准；P4.9 等评估项单独立项，不阻塞重构 |
| 线程问题的修复改变了时序（例如给 `g_page` 加锁） | 先加 debug 断言观察，再改访问方式；用“滚动 + setData”的压力夹具做回归 |

## 8. 度量指标

| 指标 | 现状 | 目标 |
|---|---|---|
| runtime 中最长的函数 | 1062 行（`BuildWidget`） | ≤ 200 行 |
| runtime 中超过 1000 行的文件 | 6 个 | 0（生成文件除外） |
| 直接调用 `FindCallback` 的位置 | 210 | ≤ 5 |
| 可变全局量 / 互斥锁 | 约 50 / 2 | 收敛到 `RuntimeState`，跨线程访问全部有锁或使用原子量 |
| 有测试校验的 FFI 哈希常量 | 0 / 143 | 全部（由生成保证） |
| 不依赖引擎的 CSS / 布局单测 | 0 | 覆盖 P4.7 列出的属性族 |
| 有结构化 golden 的夹具 | 0 / 60 | 60 / 60 |
| L5 夹具与基线的集合校验 | 无（静默跳过） | 强制一致 |
| CI 中真正执行的运行时测试 | 0 | `skyline_layout_cli` golden + 构建指纹校验；引擎测试在本机或自托管 runner 上运行 |
| bootstrap 中的空 `catch {}` | 31 | 0 |
| addon / host 中手写的 runtime `extern "C"` 声明 | 34 个 + weak 声明 | 0 |
| 未写入文档的 native 环境变量 | 7 / 16 | 0（参考文档由注册表生成） |

## 9. 里程碑

- **M1（P0 完成）**：本机全绿，L5 集合一致，回归已修复。
- **M2（P1 + P2 完成）**：过期产物不会再静默生效；60 个夹具都有 golden；ops 测试已
  按领域拆分。从这里开始才动 P3、P4、P5。
- **M3（P3 完成）**：边界有了契约，日志和配置统一。
- **M4（P4.1–P4.6 完成）**：数据模型与回放会话统一。
- **M5（P4.7–P4.8 与 P5 完成）**：core 库和单测进入 CI，巨型函数拆分完成，
  bootstrap 完成模块化。
- **M6（P6 完成）**：架构与工作流文档定稿，README 精简完成。

可以并行的部分：P1 与 P2 可以同时进行；P4（C++ 侧）与 P5（JS 侧）可以并行；P6 中
修正类的任务随时可做。

## 附录 A：重建矩阵（现状）

| 修改内容 | 需要按顺序执行 |
|---|---|
| `native/runtime/*.cc`、`*.hh`、`sources.txt` | `runtime:embed`（debug）与 `runtime:embed -- --runtime-mode release`（二者都会重建引擎）→ `native:build` 与 `native:build:release` → `devtools:stage`；导出签名有变化时还需要 `kernel:skyline` 与 `devtools:aot` |
| `native/engine/patches/*`、`dart_ui_compat/*` | `ui:patch` → `kernel:skyline` → `engine:build`（debug 与 release）→ `native:build*` → `devtools:aot` → `devtools:stage` |
| `native/engine/app_compat/*.patch`（P1 之前为 `apply.py`） | `kernel:skyline` → `devtools:aot` → `devtools:stage` |
| `native/src/**`（含 bootstrap）、`native/host/**` | `native:build` 与 `native:build:release` → `devtools:stage`（bootstrap 分片已登记为 CMake 配置依赖，构建时会自动重新生成） |
| 重新执行 `engine:fetch` | 引擎 checkout 中的 `BUILD.gn` 与导出表修改会被还原：`ui:patch` → `runtime:embed`（两种模式）→ 重建引擎 |
| 新增或修改夹具 | `l5:baseline`；如需 Windows 对照，执行 `l5:windows -- --only <name> --write` |

P1 之后，`npm run build:status` 会根据实际过期的步骤给出这张表中对应的命令。

## 附录 B：native 代码读取的环境变量

`SKYLINE_APP_SERVICE_DEBUG`、`SKYLINE_APP_SERVICE_OPS`、`SKYLINE_STUB_PAGE`、
`SKYLINE_STUB_PAGE_JSON`、`SKYLINE_DUMP_OPS`、`SKYLINE_LAYOUT_DEBUG_ID`、
`SKYLINE_SNAPSHOT_DIR`、`SKYLINE_RESOURCE_ROOT`、`SKYLINE_ASSETS`、
`SKYLINE_AOT_LIBRARY`、`SKYLINE_ICU`、`SKYLINE_DEVTOOLS`、
`SKYLINE_DEVTOOLS_DEMO_OPS`、`SKYLINE_POINTER_VIEW_ID`、
`SKYLINE_SHARED_MEMORY_LOG`、`SKYLINE_SHARED_MEMORY_HANDLE`。

其中 `SKYLINE_APP_SERVICE_DEBUG` 有 106 处读取点，大多位于各个调试分支中。P3 的
`Config` 注册表落地后，这一附录由 `npm run env:doc` 生成。

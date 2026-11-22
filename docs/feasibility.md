# 可行性分析：Skyline 渲染模块的 Linux 移植

本文只记录可复现的二进制证据与结论，不包含猜测性的“应该可以”。

## 1. 被测对象

官方微信开发者工具（2.02.2609xxx，Windows x64）中相关文件：

```
resources/app.asar.unpacked/node_modules/
├── skyline-addon/
│   ├── build/skyline.node          18 MB  PE32+ x64 DLL
│   ├── build/icudtl.dat            10 MB
│   ├── bundle/libapp.so            8.8 MB AOT 快照（ELF 容器）
│   ├── bundle/flutter_assets/      kernel_blob.bin 70 MB、字体、图标等
│   ├── index.js                    module.exports = require('./build/skyline.node')
│   └── package.json
└── sharedMemory/
    ├── sharedMemory.node           279 KB PE32+ x64 DLL
    └── smem.dll                    60 KB
```

`skyline.node` 的 `index.js` 不导出任何具名接口；模块加载的副作用是向 V8 全局注入：

```js
global.SkylineShell      // 构造函数，31 个原型方法
global.SkylineDebugInfo  // { skyline_git_rev, flutter_engine_git_rev, skyline_version }
```

实测值（官方 2.02.2609250）：

```json
{
  "skyline_git_rev": "ef3edf6c94265dfe314f9197ff4e100f99aac070",
  "flutter_engine_git_rev": "6b917cf86ebe1b9ff148310504a736d667527e8b",
  "skyline_version": "1.4.21"
}
```

## 2. 二进制证据

### 2.1 静态链接了 Flutter Engine

`skyline.node` 的导出表包含：

```
Dart_Initialize / Dart_CreateIsolateGroup / Dart_Invoke / ...
FlutterDesktopCreateWindow / FlutterDesktopDispatchTouchStartEvent /
FlutterDesktopReadPixelsOnWindow / FlutterDesktopRunEngine / ...
GetFlutterEngineGitRevision / GetSkylineGitRevision / GetSkylineVersion /
GetSkylineUserAgent / InitDartAPIDLSkyline
```

内嵌源码路径字符串：

```
../../flutter/lib/ui/skyline_ext/skyline_ext.cc
../../flutter/lib/ui/skyline_ext/skyline_normal_image_impl.cc
../../flutter/lib/ui/skyline_ext/snapshot/skyline_snapshot_impeller.cc
../../flutter/shell/common/animator.cc
../../flutter/impeller/...
```

说明它是**定制 Flutter Engine**，不是系统里的 `libflutter_linux_gtk.so` 能替代的。

### 2.2 静态链接了 Skyline 运行时

同一 DLL 内的源码路径：

```
../../skyline/renderer/flutter_engine.cc
../../skyline/renderer/window.cc
../../skyline/renderer/ffi.cc
../../skyline/runtime/engine.cc
../../skyline/runtime/engine/devtools_v8_engine.cc
../../skyline/shadow/...
../../skyline/layout/layout_paragraph.cc
../../skyline/platform/devtools/devtools_shell.cc
../../skyline/platform/devtools/flutter_window.cc
../../skyline/platform/devtools/embedder_devtools.cc
```

日志前缀也来自这些文件（`flutter_window.cc(142) shared_buffer_size: ...`、
`devtools_shell.cc(148) window_id: ...`）。Skyline 的布局、绘制、V8 运行时全部在
这个 DLL 内。

### 2.3 AOT 快照面向 Windows

`bundle/libapp.so` 的 `file` 结果是 ELF x86-64（Dart AOT 使用 ELF 容器），但快照内的
构建标记是：

```
_<DartSnapshotBuildId> ...
f04e2c154dfcdcf7e92dfa2fc58f4747product no-code_comments ... x64 windows no-compressed-pointers
```

即：**目标平台是 Windows**。它不能直接交给 Linux 版 Flutter Engine 执行。

`bundle/flutter_assets/kernel_blob.bin`（70 MB）是平台无关的 Dart kernel，可在 JIT
引擎中运行，但该应用依赖定制引擎的 `skyline_ext` 与 DLL 导出的 FFI 符号；换成官方
Flutter Engine 会缺少这些符号。

### 2.4 与 DevTools 的胶水代码

`resources/app.asar` 中 `js/extensions/appservice/index.js` 的 `createSkylineShell`
（节选，已还原）：

```js
const shared = safeSkylineRequire('sharedMemory/sharedMemory.node');
const bytes = width * dpr * height * dpr * 4 * 4 + 4 + 4;
const descriptor = shared.setMemory(sharedMemoryKey, bytes);   // Windows: 16 字节指针/长度
const args = [window_id, bundlePath, width, height, dpr,
              !showWindow, buffer, addonPath];                 // 8 个
if (sharedMemoryAddress) args.push(sharedMemoryAddress);       // Electron 下追加第 9 个
new SkylineShell().createWindow(...args);
```

窗口就绪回调链：

```
setNotifyBootstrapDoneCallback  → 放行排队的 createWindow Promise
setNotifyWindowReadyCallback    → resolve 单个窗口
onWindowRasterized(windowId, cb)→ 发送 SPC1 给渲染层读取共享内存
```

渲染层 `js/extensions/skyline/index.js` 的帧消费：

```js
// 外层（本模块）头 16 字节：u64 size + i32 version
// 数据区前 8 字节：u32 frameId + u32 frameSize (LE)
// 之后为 frameSize 字节的 RGBA8，尺寸 ceil(w*dpr) * ceil(h*dpr) * 4
estimateSharedMemoryBytes(w, h, dpr) = 8 + ceil(w*dpr) * ceil(h*dpr) * 4
```

### 2.5 共享内存模块

`sharedMemory.node` 与 `skyline.node` 是两个独立模块。官方 Windows 版基于
**Boost.Interprocess**：

- `setMemory(key, size)` 返回 16 字节 `Uint8Array`（LE：8 字节进程内指针 + 8 字节
  长度），供 `skyline.node` 在同一进程内直接访问；
- `getMemory(key)` 返回数据区的 `Buffer` 视图；
- 原生 Windows 使用命名共享对象；Wine 把对象落成
  `drive_c/ProgramData/boost_interprocess/<bucket>/<key>`，**文件内容只有数据区**，
  没有额外头；
- 官方还导出 `copyMemoryToBuffer` / `deleteCopyCache` / `getMemoryByAddress` /
  `setMemoryByAddress` / `deleteSetCache` / `deleteGetCache`，不导出 `version`。

Linux 实现（`skyline-shared-memory`）使用 `/dev/shm/skyline_<key>.dat`，文件格式为
16 字节外层头（`u64 size + i32 version + 4 字节填充`）+ 数据区，并通过
`copyMemoryToBuffer(key, target)` 把数据区复制到 JS 分配的 Buffer，规避 Electron 的
V8 内存限制。

两者对外的**数据区**一致：帧头（`u32 frameId + u32 frameSize`）+ RGBA8 像素。
`test/cross.test.js` 已实测双向互通：Windows 写出的 Boost 数据区与 Linux 期望逐字节
相同；Linux 数据区落成 Boost 文件后，官方 Windows 模块能打开并读回相同内容。

## 3. 关键实验：最小宿主中的 `createWindow`

在 Wine 11 + 官方 Electron 36.6.0 中加载官方模块并调用
`createWindow(10, bundle, 100, 100, 2, true, null, addonPath, descriptor)`：

- 模块加载、全局对象、方法表、`SetSafeAreaEdgeInsets`、共享内存分配均正常；
- `dpr` 会被 `Number()` 化：传 `'2'` 时日志仍打印 `device pixel ratio: 2`；
- 相对 `bundle` 路径按 Electron 可执行文件目录解析（日志出现
  `<electron>/bundle/libapp.so`）；绝对路径才会指向传入位置；
- 绝对 bundle 下 Flutter Engine 成功初始化，`flutter_window.cc` 打印
  `shared_buffer_size: 640008`、`create window id : 10`；
- Dart 应用开始执行（`devtools.dart main args [...]`、`WaLoad Runtime::Runtime`），
  随后在 Node 初始化路径崩溃：

```
#  Node.js[32]: IsolatePlatformDelegate *__cdecl
   node::NodePlatform::ForIsolate(Isolate *)
   at ..\..\third_party\electron_node\src\node_platform.cc:649
#  Assertion failed: (data.first) != nullptr
```

复现范围：`ELECTRON_RUN_AS_NODE`、Electron 主进程、`<webview>` guest（已按官方
webpreferences 配置）三种宿主均复现。官方完整 DevTools 环境（appservice guest 由
DevTools 自身创建）不在此列，需要进一步复刻其初始化顺序。

补充观测（无有效 bundle 时）：`FATAL: flutter_window.cc(91) Check failed:
window_controller_`，并提示 `Unable to start engine without AOT data`。这说明
`createWindow` 的失败路径也可能以 fatal 终止进程，因此测试必须逐用例隔离子进程。

结论：**最小宿主中的 `createWindow` 断言已通过“渲染进程 + 窗口就绪后注入”规避**；
L1–L4 不受影响。该断言不影响“官方 Windows 模块能提供基准数据”这一事实——模块加载、
参数校验、共享内存与帧协议均可在当前环境完整观测。引擎级像素采集的后续结论见 §3.1。

### 3.1 2026-11-20：真实 L5 采集已跑通

C1 原先按“需要完整官方 DevTools 环境”阻塞。实际验证表明，**官方引擎的像素输出不需要
完整 DevTools UI**，只需要正确的宿主初始化顺序：

1. 在 Electron **渲染进程**（`nodeIntegration`、`contextIsolation:false`）中加载官方
   `skyline.node`，构造 `SkylineShell` 并注册回调；
2. 调用 `createWindow(windowId, bundle, width, height, dpr, true, null, addon, descriptor)`；
   `dpr` 必须为 **2**，否则命中 `flutter_window.cc(97) Check failed:
   device_pixel_ratio == 2`；
3. 等待 `setNotifyWindowReadyCallback`。此时官方 addon 才把 `SkylineGlobal` 注入当前
   渲染进程的 V8；`createWindow` 返回时它仍为 `undefined`；
4. 调用 `skylineShell.notifyAppLaunch(windowId, 0, { backgroundColorContent })`，让 Dart
   侧建立页面路由；
5. 用 `new SkylineGlobal.PageContext(0, windowId)` 创建上下文，通过
   `createElement`/`setStyle`/`addClass`/`appendChild`/`attach`/`startRender` 回放
   appservice op 流；
6. 从共享内存读取 `frameId + frameSize + RGBA`。**Windows 原始帧是 BGRA**（官方
   pageframe 的 WebGL shader 在 win32 下使用 `.bgra`），需交换 R/B 后再比较。

`tools/l5-windows.js` 与 `tools/l5-win-app/` 实现了上述流程；`npm run l5:windows`
采集并将结果写入 `spec/l5-baseline.win32.json`。官方引擎限制 dpr=2，因此帧为
640x480 设备像素；Linux 侧用同一夹具在 `--dpr 2` 下重放，比较时仅排除 Linux debug
构建右上角的 “DEBUG” 横幅（`ignoreRects`，release Windows 引擎没有该横幅）。

25 个夹具的实测结果（`npm run test:l5`，容差 12/通道）：

| 夹具 | 结果 |
| --- | --- |
| stripes/fixed/transform-origin/align-gap/flex-wrap/order/absolute/flex-basis/box-sizing/percent-box/sticky/aspect-ratio/map | 逐像素完全一致（maxDelta=0） |
| shadow/radial-conic/background-size | maxDelta=1 |
| overflow | maxDelta=109，失配率 0.14%（裁剪边缘） |
| transform3d | 失配率 18%，Linux 3D 变换保真差异（记录为已知差异） |
| transform-origin-z | 失配率 10%；官方引擎忽略 transform-origin 的 z 分量（已知差异） |
| vertical-align | 失配率 0.8%；inline-block 行盒布局已对齐，剩余为文本栅格化差异 |
| text-overflow | 失配率 1.6%；官方在该 op 流下同样直接裁剪，剩余为文本栅格化差异 |
| white-space | 失配率 2.3%；nowrap/normal 切换已对齐，剩余为 Skyline breakAll 与 Flutter 单词换行的差异 |
| gridview/listview | 合成 op 流不会触发官方 native builder 回调，标记不可比 |

D6 后 `aspect-ratio`、`display:inline-block` 行盒、`white-space` 模式和
`text-overflow` 解析均已实现；`white-space` 的剩余差异来自 Skyline 默认
`WordBreak.breakAll`（该 Flutter 文本路径不暴露该开关），`text-overflow` 的
剩余差异是文本栅格化。

`map` 在 L5 合成 op 流中两边都是空白占位（maxDelta=0），因此“原生地图内容”
仍未被该夹具覆盖；`pointer-events`/`will-change` 无静态像素差异，留待行为探针。

完整官方 DevTools UI 在 Wine 下启动后卡在登录/主线程同步 IPC；这不是 L5 采集的
必要条件，`tools/l5-windows.js` 的渲染进程路径已覆盖 C1 的“官方引擎真实帧”目标。

### 3.2 2026-11-22：engine-context 契约用例补录

`spec:record` 原先用 `ELECTRON_RUN_AS_NODE`，官方模块在 `createWindow` 启动引擎时
会命中 `NodePlatform::ForIsolate` 断言，因此 24 个 engine-context 用例只能标为
`skip`。现在复用 §3.1 的渲染进程宿主：

1. `tools/win-oracle-app/` 在官方 Windows Electron 渲染进程中加载官方
   `skyline.node`；`test/lib/win-shim.js` 把 `resources/app` 指向该宿主（官方
   Electron 是打包应用，会忽略 CLI 传入的 app 路径）。
2. 对 brightness/destroy/notify 这类不建窗口的用例，先用
   `setMemory` + `createWindow(424242, …, dpr=2)` 建一个上下文窗口，并注册
   `LoadResource/LoadResourceAsync/HttpRequest` 等回调（缺少这些回调时官方引擎
   会在初始化期间崩溃）。
3. 用例仍由 `test/driver.js#executeCase` 执行，主进程把结果写回原来的
   `--out` JSON，因此 `test/lib/runner.js` 不需要第二条结果通道。

结果：60 个用例中 50 个可观测，10 个官方进程崩溃（FATAL/访问违例；快照只记录
归一化 `reason`，完整日志留在 `tmp/results/`）；新增 14 个 Linux/Windows 可直接
对比的用例。`test:native` 现在为 53 通过 / 10 跳过 / 0 失败。崩溃用例（如
`dpr=0/1/3`、无效 descriptor）仍由契约测试跳过，因为官方进程无法生成稳定基准。

## 4. 原生移植的可行性

### 方案 A：Wine 托管官方引擎 + Linux 兼容层（可落地）

- Linux `skyline.node` 只实现 JS 侧契约（`SkylineShell`/`SkylineDebugInfo`），
  调用转发到 Wine 进程内的官方引擎；帧仍经 `/dev/shm` 交换。
- 优点：渲染结果与 Windows 完全一致；复用现有 Wine 环境与 `skyline-shared-memory`。
- 缺点：依赖 Wine 与 Windows 版 Electron；启动/内存开销较高。
- 前置条件：解决/绕过 3 节的 `ForIsolate` 断言，或复用官方 DevTools 的完整 guest 环境。

### 方案 B：原生重写引擎（已选定，分阶段推进）

- 需要 `skyline/*` 与定制 Flutter Engine 的源码；二者均未公开（已检索 GitHub /
  公开镜像，未找到 `devtools_shell.cc`、`SkylineNormalImageImpl` 等符号的来源）。
- 实施方式：用公开 Flutter Engine + 反编译/行为观测复刻 `skyline/*`；AOT 快照面向
  Windows，必须先由 `kernel_blob.bin` 重新生成 Linux 快照。
- **决策（2026-11-19）：选择该路线。** 详细任务与验收标准见
  [native-roadmap.md](native-roadmap.md)。阶段划分：
  1. Native N-API API 层（与 `reference/` 契约一致，可独立测试）；
  2. Linux Flutter Engine 构建与 `dart:ffi` / 平台通道打通；
  3. 反编译 Skyline 运行时（layout/paint/runtime/V8 边界）；
  4. 帧输出接入 `skyline-shared-memory`，启用 L5 像素对比。
- 风险：工程量以人年计；在阶段 1/2 完成前，Wine 官方引擎仍是唯一完整的渲染基准。

### 方案 C：只做契约/协议测试（本次交付的基础）

- 先固定 L1–L4 行为基线，为 A/B 任一实现提供回归保护。
- 已实现 `reference/skyline-node.js`：精确复刻官方 JS 可见契约（全局对象、方法顺序、
  描述符、参数校验、错误消息），但不含引擎；L1/L2 差分测试默认以它为被测目标。
- 已实现共享内存 L3/L4 与跨平台字节级互通测试。
- 当前结果：reference 目标（无引擎）跳过引擎上下文用例；`npm run test:native`
  在原生 addon 上为 53 通过 / 10 跳过（官方进程崩溃、无稳定基准）/ 0 失败，
  其中 14 个 engine-context 用例由 Wine 渲染进程 oracle 补录。

## 5. 测试策略

1. **同一份用例、两个运行器**：`test/cases/*` 不区分平台；Windows 运行器通过
   Wine + 官方 Electron 执行，Linux 运行器直接 `node` 执行。
2. **崩溃隔离**：每个可能触发 native 崩溃的用例单独子进程，结果逐条落盘，崩溃
   不影响其他用例。
3. **归一化后逐条 deepEqual**：路径、指针、版本号、堆栈等易变字段归一化；其余字段
   必须完全一致。
4. **共享内存互通**：Linux 写入 / Wine 读取，Wine 写入 / Linux 读取，验证外层头、
   帧头、零初始化与代际替换。
5. **引擎级像素对比**：作为 `test/engine.test.js` 的受控集成测试，默认跳过；在
   官方 guest 环境可复现后启用。

## 6. 复现命令

```bash
# 记录官方模块契约（Wine）
SKYLINE_WIN_RESOURCES=.../app.asar.unpacked \
SKYLINE_WIN_ELECTRON=.../electron.exe npm run spec:record

# Linux 对比
SKYLINE_LINUX_SKYLINE_ADDON=.../skyline.node \
SKYLINE_LINUX_SHARED_MEMORY=.../sharedMemory.node npm run test:contract
```

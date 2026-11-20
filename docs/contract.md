# Skyline Node 模块契约

本契约以官方 Windows x64 模块（开发者工具 2.02.2609250）的实测结果为准。任何 Linux
实现必须逐项一致；测试用例位于 `test/cases/`，快照位于 `spec/`。

## 1. 模块加载

```js
require('sharedMemory/sharedMemory.node');          // 先加载共享内存模块
require('skyline-addon/build/skyline.node');        // 无返回值、无具名导出
```

加载后的全局副作用：

| 名称 | 类型 | 说明 |
| --- | --- | --- |
| `global.SkylineShell` | function | 构造函数，`name === ""`，`length === 0`，`Object.keys() === []` |
| `global.SkylineDebugInfo` | object | `{ skyline_git_rev, flutter_engine_git_rev, skyline_version }`，三个字段均为 string |
| `global.SkylineWindow` | undefined | 不注册 |

全局属性描述符：`enumerable: true, writable: true, configurable: true`。

`SkylineShell` 自身没有静态成员（除 `length/name/prototype`），`prototype` 上只有
`constructor` + 下列方法。实例没有自有属性。

## 2. 方法表

顺序即官方 `Object.getOwnPropertyNames(prototype)` 顺序（`constructor` 最后）：

| # | 方法 | `length` | 说明 |
| --- | --- | --- | --- |
| 1 | `setNotifyBootstrapDoneCallback` | 0 | 引擎 bootstrap 完成 |
| 2 | `createWindow` | 0 | 创建渲染窗口，同步返回 `null` |
| 3 | `destroyWindow` | 0 | 销毁窗口 |
| 4 | `onWindowRasterized` | 0 | 注册每窗口光栅化回调 |
| 5 | `setNotifyWindowReadyCallback` | 0 | 窗口就绪 |
| 6 | `setNotifyRouteDoneCallback` | 0 | 路由完成 |
| 7 | `setNavigateBackCallback` | 0 | 返回事件 |
| 8 | `setNavigateBackDoneCallback` | 0 | 返回完成 |
| 9 | `setLoadResourceCallback` | 0 | 同步资源加载 |
| 10 | `setLoadResourceAsyncCallback` | 0 | 异步资源加载 |
| 11 | `setHttpRequestCallback` | 0 | HTTP 请求 |
| 12 | `setSendLogCallback` | 0 | 日志上报 |
| 13 | `setSafeAreaEdgeInsets` | 0 | left/top/right/bottom |
| 14 | `notifyResourceLoad` | 0 | 异步资源结果 |
| 15 | `notifyHttpRequestComplete` | 0 | HTTP 结果 |
| 16 | `dispatchTouchStartEvent` | 0 | |
| 17 | `dispatchTouchMoveEvent` | 0 | |
| 18 | `dispatchTouchEndEvent` | 0 | |
| 19 | `dispatchTouchOverEvent` | 0 | |
| 20 | `dispatchTouchCancelEvent` | 0 | |
| 21 | `dispatchWheelEvent` | 0 | |
| 22 | `dispatchCharEvent` | 0 | |
| 23 | `dispatchKeyboardEvent` | 0 | |
| 24 | `notifyNavigateTo` | 0 | |
| 25 | `notifyRedirectTo` | 0 | |
| 26 | `notifyNavigateBack` | 0 | |
| 27 | `notifyAppLaunch` | 0 | |
| 28 | `notifyReLaunch` | 0 | |
| 29 | `notifyAutoReLaunch` | 0 | |
| 30 | `notifySwitchTab` | 0 | |
| 31 | `onPlatformBrightnessChanged` | 0 | 1=LIGHT, 2=DARK |

所有方法：`enumerable: true, writable: true, configurable: false`，且原生
`Function.length === 0`（不要依赖 JS 层 arity 做参数校验）。

## 3. 官方调用序列（DevTools 侧）

```js
const shell = new SkylineShell();               // 只创建一次，全局共享
shell.setNotifyBootstrapDoneCallback(cb);       // 可在 createWindow 前
shell.setSafeAreaEdgeInsets(l, t, r, b);
shell.setLoadResourceCallback(fn);              // 返回 Buffer
shell.setLoadResourceAsyncCallback(fn);         // 完成后调用 notifyResourceLoad
shell.setHttpRequestCallback(fn);               // 完成后调用 notifyHttpRequestComplete
shell.setNotifyWindowReadyCallback(fn);
shell.setNotifyRouteDoneCallback(fn);
shell.setNavigateBackCallback(fn);
shell.setNavigateBackDoneCallback(fn);
shell.setSendLogCallback(fn);

const shared = require('sharedMemory/sharedMemory.node');
const bytes = width * dpr * height * dpr * 4 * 4 + 4 + 4;
const handle = shared.setMemory(key, bytes);    // Windows: 16 字节 Uint8Array
const args = [windowId, bundlePath, width, height, dpr,
              !showWindow, buffer, addonPath];  // 8 个
if (handle) args.push(handle);                  // Electron 路径：9 个
shell.createWindow(...args);                    // 返回 null
shell.onWindowRasterized(windowId, cb);         // 必须在 createWindow 之后
```

`bundlePath` 指向 `bundle/`（含 `libapp.so`、`flutter_assets/`）；`addonPath` 指向
`build/skyline.node`。`dpr` 在官方实现中只接受 `2`（`flutter_window.cc(97)` 的
`Check failed: device_pixel_ratio == 2`）。debug/JIT 引擎使用
`flutter_assets/kernel_blob.bin`；release 引擎使用 `bundle/libapp.so`，通过
`FlutterEngineCreateAOTData` + `FlutterProjectArgs.aot_data` 加载。

阶段 2 的 Linux 引擎后端（`native/host/host_engine.cc`）接受两种帧目标：

- 6 号参数为 `ArrayBuffer` / typed array：其内存直接作为数据区
  （`frameId/frameSize/RGBA8`），适合 Linux `setMemory` 返回的数据视图；
- 9 号参数为官方 16 字节描述符（LE64 ptr + LE64 size）：指针位于本进程
  共享内存时同样可用。

每帧写完后通过 `onWindowRasterized(windowId, cb)` 回调通知；bootstrap 要求
`createWindow` 至少 9 个参数，因此只走 6 号参数时也要传入一个 16 字节占位
descriptor（全零即可）。

## 4. 共享内存

官方 Windows 模块与 Linux 移植使用不同的后端，但对外暴露的**数据区**必须逐字节一致：

| | 官方 Windows | Linux 移植 |
| --- | --- | --- |
| 后端 | Boost.Interprocess `shared_memory_object` | POSIX 文件 + `mmap` |
| 物理位置 | 原生 Windows 为命名对象；Wine 下为 `drive_c/ProgramData/boost_interprocess/<bucket>/<key>` | `/dev/shm/skyline_<key>.dat` |
| 文件内容 | **只有数据区**（无外层头） | 16 字节外层头 + 数据区 |
| `setMemory` 返回 | 16 字节 `Uint8Array`（ptr LE64 + size LE64） | external `ArrayBuffer`（数据区视图） |
| `getMemory` 返回 | `Buffer`（数据区视图） | external `ArrayBuffer`（数据区视图） |
| 缺失 key 的 `getMemory` | 返回 `null` | 返回 `null`（2026-11-19 对齐） |

外层头只属于 Linux 文件格式，用于按 key 打开时获知长度；公共 API 不得暴露它——
`getMemory` / `copyMemoryToBuffer` 的起点始终是数据区（即帧头）。

### 4.1 外层头（Linux 文件格式，16 字节）

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | `uint64` LE | 数据区字节数 |
| 8 | `int32` LE | 版本号，首次创建为 `1` |
| 12 | 4 字节 | 对齐填充 |

数据区起始于文件偏移 16。`setMemory` 必须把数据区清零。

### 4.2 帧头（数据区偏移 0，共 8 字节）

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | `uint32` LE | `frameId` |
| 4 | `uint32` LE | `frameSize`（RGBA 字节数） |

像素从数据区偏移 8 开始，格式 RGBA8，尺寸 `ceil(width*dpr) × ceil(height*dpr)`。

### 4.3 分配大小

```
bufferBytes = width * dpr * height * dpr * 4 * 4 + 8
```

即 4 份 RGBA 帧 + 8 字节帧头预算。渲染层实际需要
`8 + ceil(width*dpr) * ceil(height*dpr) * 4`，读端必须容忍生产端更大的分配。

### 4.4 实测差异与判定

| 行为 | 官方 Windows（Wine） | Linux 现有实现 | 判定 |
| --- | --- | --- | --- |
| `setMemory` 返回 | 16 字节 `Uint8Array` 描述符 | external `ArrayBuffer` | 平台 ABI，允许 |
| `getMemory` 返回 | `Buffer` | external `ArrayBuffer` | 数据类型不同，`byteLength` 必须一致 |
| `version` 导出 | 无 | 有 | 允许 |
| `copyMemoryToBuffer` / `deleteCopyCache` | 有 | 有 | 必须一致 |
| `getMemoryByAddress` 等地址接口 | 有 | 无 | 允许（仅 Windows 进程内指针） |
| 合法 key 字符集 | `../escape`、空格、中文、201 字符均接受 | 仅 `[A-Za-z0-9._-]` 且 ≤200 | **有意收紧**（防路径穿越）；安全 key 行为一致 |
| 缺失 key 的 `getMemory` | 返回 `null` | 返回 `null` | 已对齐（见 4.5） |
| `removeMemory` 返回值 | `undefined` | `undefined` | 已对齐（见 4.5） |
| 非法 length 的异常 | `Error` | `RangeError` | 同属失败路径，测试按错误族接受 |
| 超大 length | 会尝试映射，可能挂起 | `> 1 GiB` 立即拒绝 | 测试不覆盖超大分配 |

Linux 目标实现若作为官方 JS 的 drop-in 替换，必须在 Electron UA 下提供与 Windows
一致的 16 字节描述符语义（描述符只在本进程内有效）；若沿用渲染层现有路径，则保留
external `ArrayBuffer`。两种模式由实现文档明确，测试按平台期望断言，不做跨平台等值
比较。

### 4.5 缺失 key 语义（已对齐）

`js/extensions/skyline/index.js` 的 `getSharedMemoryLegacy` 先调用 `getMemory(key)`，
再判断返回值是否为假。官方 Windows 返回 `null`。2026-11-19 已修改
`skyline-shared-memory`：

- `getMemory(missing)` 返回 `null`，不再抛 `Error`；
- `removeMemory(existing|missing)` 返回 `undefined`，不再返回布尔值。

差分测试 `shm.missingKey` 与 `shm.removeExisting` 现已按严格相等判定。

### 4.6 跨平台互通（已实测）

- 官方 Windows 在 Wine 下的 Boost 文件**只包含数据区**；Linux 文件为 16 字节外层头
  + 同一数据区。
- `test/cross.test.js` 验证：Windows 写出的数据区与 Linux 期望逐字节一致；Linux
  写出的数据区复制为 Boost 文件后，官方 Windows 模块可通过 key 打开并读回相同
  SHA-256。

## 5. 帧通知协议

主进程 → 渲染层：

```
channel: PAGEFRAME|<windowId>
command: SKYLINE_WINDOW_RASTERIZED ("SPC1")
data: { windowId, args }
```

渲染层收到后重新拷贝共享内存；`destroyWindow` 时发送 `SPC2`。帧消费语义：

- 只有 `frameId` 或 `frameSize` 变化时才重新上传纹理；
- 拷贝失败且错误带 `requiredSize` 时扩容重试，最多 `MAX_COPY_RETRIES` 次；
- `requiredSize` 由 `copyMemoryToBuffer` 在目标容量不足时以 `RangeError` 属性给出。

## 6. 兼容性要求（测试判定标准）

1. L1：模块加载后的全局对象、方法表顺序、描述符、类型完全一致。
2. L2：相同非法输入的异常类型与消息一致（路径归一化后）；合法调用的返回值一致。
3. L3：共享内存头布局、零初始化、key 校验、跨进程可见性、代际替换一致。
4. L4：帧头布局、帧大小、跨平台读写互通一致。
5. L5（受控）：确定性页面的帧哈希/像素容差一致。

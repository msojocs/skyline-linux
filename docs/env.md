# 环境变量参考

> 由 `npm run env:doc` 从 `tools/env-vars.json` 生成，请勿手工编辑。
> 新增或修改变量时编辑注册表，再重新生成本文件与
> `native/include/skyline/config_vars.inc`；`test/env-registry.test.js`
> 检查代码读取的每个 `SKYLINE_*` 变量都已注册。

原生组件（runtime、addon、host、shared-memory）通过 `skyline::config`
（`native/include/skyline/config.h`）读取变量：每个进程在第一次读取时取一次
环境快照，之后修改环境不再生效。`flag` 类型除未设置、空串、`0`、`false`、
`off`、`no` 之外都视为开启。

## runtime

嵌入式运行时（libflutter_engine.so 中的 native/runtime，及独立 libskyline.so）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_APP_SERVICE_OPS` | string | — | runtime | Dart 应用就绪后回放的录制 ops：`@<文件>` 或内联 JSON。独立 skyline_host 测试通过它加载夹具。 |
| `SKYLINE_STUB_PAGE` | string | — | runtime | 合成演示页：`text`、`card` 或任意值（条纹页）；以 `{` 或 `@` 开头时视为页面描述 JSON。 |
| `SKYLINE_STUB_PAGE_JSON` | string | — | runtime | 合成演示页的描述 JSON（`@<文件>` 或内联），优先于 SKYLINE_STUB_PAGE。 |
| `SKYLINE_DUMP_OPS` | path | — | runtime | 把每批实时 ops 写入该目录（`ops-NNN.json`），用于把 DevTools 中的页面转成夹具。 |
| `SKYLINE_LAYOUT_DEBUG_ID` | int | `0` | runtime | 输出该节点 id 的测量与布局细节（0 表示不输出）。 |
| `SKYLINE_LAYOUT_DUMP` | path | — | runtime | 结构化布局快照（layout golden）的追加输出文件，见 tools/layout-golden.js。 |
| `SKYLINE_SNAPSHOT_DIR` | path | — | runtime | takeAppSnapshot / snapshotNode 的结果同时写入该目录。 |
| `SKYLINE_RESOURCE_ROOT` | path | — | runtime | 相对图片、字体等资源路径的根目录。 |

## addon

skyline.node（native/src 与 native/host/flutter_window_backend.cc）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_ASSETS` | path | — | addon | 覆盖 createWindow 使用的 flutter_assets 目录。 |
| `SKYLINE_AOT_LIBRARY` | path | — | addon | createWindow 强制使用的 AOT 快照（libapp.so），即使 bundle 中有 kernel_blob.bin。 |
| `SKYLINE_ICU` | path | — | addon | bundle 中没有 icudtl.dat 时使用的 ICU 数据文件。 |
| `SKYLINE_DEVTOOLS` | flag | — | addon | 强制按 devtools.dart 的入口参数启动（默认只在路径包含 skyline-addon 时这样做）。 |
| `SKYLINE_DEVTOOLS_DEMO_OPS` | path | — | addon | createWindow 之后立即回放的 ops 文件，用于在没有 appservice 桥时演示页面。 |

## host

独立宿主 skyline_host（native/host/main.cc）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_POINTER_VIEW_ID` | int | `0` | host | --pointer-script 事件使用的 Flutter view id。 |

## shared-memory

sharedMemory.node（native/shared-memory）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_SHARED_MEMORY_LOG` | flag | — | shared-memory | 输出 sharedMemory.node 的调试日志。 |
| `SKYLINE_SHARED_MEMORY_HANDLE` | flag | — | shared-memory | 强制 Electron 句柄模式：setMemory 返回 16 字节指针/长度描述符（appservice 删除了 `process`，自动检测会退回 UA 判断）。 |

## native

全部原生组件（runtime、addon、host、shared-memory）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_APP_SERVICE_DEBUG` | flag | — | native | 调试开关：输出回放、手势、滚动与语义树等调试日志。 |

## dart

Dart 侧：package:sample（native/engine/app_compat 补丁）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_HTTP_TEST_URL` | string | — | dart | devtools.dart 启动时用 dart:io 请求该 URL，并打印 `SKYLINE_HTTP_RESULT <字节数> <内容>`（图片网络测试）。 |

## scripts

构建脚本 native/engine/*.sh

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_ENGINE_CHECKOUT` | path | `native/engine/checkout` | scripts | Flutter 引擎 checkout 目录。 |
| `SKYLINE_DEPOT_TOOLS` | path | `native/engine/depot_tools` | scripts | depot_tools 目录。 |
| `SKYLINE_ENGINE_TOOLCHAIN` | path | `native/engine/toolchain` | scripts | 下载的 Flutter/Dart 工具链目录。 |
| `SKYLINE_ENGINE_STAGE` | path | `native/engine/out/linux-x64-debug` | scripts, tools | 引擎产物目录（libflutter_engine.so、flutter_embedder.h、patched_sdk）。 |
| `SKYLINE_ENGINE_WORK` | path | `native/engine/work` | scripts, tools | 从官方 kernel 恢复出的源码与 Skyline 应用的工作目录。 |
| `SKYLINE_PATCHED_SDK` | path | `$SKYLINE_ENGINE_STAGE/patched_sdk` | scripts, tools | 打过 dart:ui 兼容补丁的 Flutter SDK。 |
| `SKYLINE_APP_DIR` | path | `apps/empty-page` | scripts | build-empty-page.sh / build-aot.sh 构建的 Flutter 应用目录（相对仓库根或绝对路径）。 |
| `SKYLINE_PROXY` | string | — | scripts | 下载引擎与工具链时使用的 HTTP(S) 代理。 |
| `SKYLINE_PYTHON` | path | — | scripts | 引擎构建使用的 Python 3.12；未设置时优先用 uv 安装。 |
| `SKYLINE_GEN_SNAPSHOT` | path | — | scripts | build-aot.sh 使用的 gen_snapshot（默认取 release 引擎构建目录中的版本）。 |
| `SKYLINE_DEVTOOLS_DIR` | path | `../wechat-web-devtools-linux` | scripts, tests, tools | Linux 微信开发者工具目录：staging 目标、Windows 资源与录制器的查找位置。 |
| `SKYLINE_DEVTOOLS_ENGINE` | enum: release / debug | `release` | scripts | devtools:stage 打包的引擎模式。 |
| `SKYLINE_SHARED_MEMORY_NODE` | path | — | scripts | devtools:stage 改用该 sharedMemory.node，而不是仓库内构建的版本。 |

## tests

测试（test/、native/test/）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_REQUIRE_ENGINE` | flag | — | tests | 设为 1 时，依赖引擎产物的测试在产物缺失时失败而不是跳过（CI 使用）。 |
| `SKYLINE_ALLOW_STALE_BUILD` | flag | — | tests | 设为 1 时，构建产物过期的 host 测试改为跳过。 |
| `SKYLINE_L5_WINDOWS_BASELINE` | path | `spec/l5-baseline.win32.json` | tests | test:l5 使用的 Windows 基线索引。 |
| `SKYLINE_LINUX_SKYLINE_ADDON` | path | `reference/skyline-node.js` | tests | 契约测试的目标实现（test:native 使用 native/build/skyline.node）。 |
| `SKYLINE_LINUX_SHARED_MEMORY` | path | `native/build*/sharedMemory.node` | tests | 契约与共享内存测试使用的 Linux sharedMemory.node。 |
| `SKYLINE_LINUX_BUNDLE` | path | — | tests | 契约测试 createWindow 使用的 bundle 目录。 |
| `SKYLINE_LINUX_RESOURCES` | path | — | tests | 契约测试使用的资源目录（默认同 bundle）。 |
| `SKYLINE_SHARED_MEMORY_MODULE` | path | — | tests | 共享内存测试优先加载的 sharedMemory.node。 |
| `SKYLINE_RESULTS_DIR` | path | `tmp/results` | tests | 契约测试中每个用例结果文件的目录。 |
| `SKYLINE_SPEC` | path | `spec/skyline-node.win32.json` | tests | 契约测试比对的 Windows oracle 记录。 |
| `SKYLINE_DEVTOOLS_VERSION` | string | `2.02.2609250` | tests | spec:record 写入 oracle 记录的 DevTools 版本。 |
| `SKYLINE_WINE` | path | `wine` | tests, tools | 运行官方 Windows 模块使用的 wine。 |
| `SKYLINE_WIN_ELECTRON` | path | — | tests, tools | Wine 下使用的官方 DevTools electron.exe。 |
| `SKYLINE_WIN_RESOURCES` | path | — | tests, tools | 官方 Windows DevTools 的 resources/app.asar.unpacked 目录。 |
| `SKYLINE_WIN_SCENARIO` | enum: l5 / oracle | — | tests, tools | tools/win-host 的场景：L5 采帧或契约 oracle 用例。 |

## tools

工具（tools/）

| 变量 | 类型 | 默认值 | 组件 | 说明 |
| --- | --- | --- | --- | --- |
| `SKYLINE_TEST_CONCURRENCY` | int | — | tools | tools/run-tests.js 同时运行的测试文件数（默认 CPU 数的一半）。 |
| `SKYLINE_KERNEL_BLOB` | path | — | tools | 官方 kernel_blob.bin（spec:symbols、源码恢复）。 |
| `SKYLINE_ISOLATE_SNAPSHOT` | path | — | tools | 官方 isolate_snapshot_data（spec:symbols）。 |
| `SKYLINE_WIN_ADDON` | path | — | tools | 官方 Windows skyline.node（spec:symbols 读取导出表）。 |
| `SKYLINE_SKYLINE_BUNDLE` | path | — | tools | ops:record 使用的官方 Skyline 渲染 bundle。 |
| `SKYLINE_EXAMPLES` | path | `../awesome-skyline/examples` | tools | npm run gaps 扫描的 Skyline 示例目录。 |
| `SKYLINE_DEVTOOLS_RESOURCES` | path | `$SKYLINE_DEVTOOLS_DIR/resources` | tools | wxlib:style 读取的 DevTools resources 目录。 |
| `SKYLINE_WXLIB_VERSION` | string | `REVISION 中的 WXLIB_VERSION` | tools | wxlib:style 提取样式的基础库版本。 |

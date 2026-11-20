# Skyline Node 模块（Linux 原生骨架）

本目录实现 `skyline.node` 的 **Native N-API API 层**：

- 在模块加载时注册全局 `SkylineShell` / `SkylineDebugInfo`，方法顺序、属性描述符、
  参数校验与错误消息与官方 Windows 模块一致；
- 通过 `__skylineNative` 原生对象预留引擎边界（回调注册、窗口生命周期、事件分发）；
- 引擎后端：`native/build/skyline.node` 在固定引擎已构建时链接
  `libflutter_engine.so`（`SKYLINE_HAVE_ENGINE`），`createWindow` 为每个窗口启动
  独立 platform thread；未构建引擎时回退到 `StubEngine`，契约测试不受影响。

## 引擎后端（阶段 2）

`native/host/host_engine.cc` 实现 `skyline::Engine`：

- `createWindow` 解析 `bundle_path`（`flutter_assets/kernel_blob.bin` 或直接指向
  assets 目录）与 `icudtl.dat`（bundle、`SKYLINE_ICU`、引擎 stage 默认路径），
  校验 width/height/dpr，然后启动 thread 执行 `FlutterEngineRun` + implicit view
  metrics；启动失败同步抛错。
- 出帧：`surface_present_callback` 把 BGRA 转成 RGBA，写入调用方数据区
  （6 号参数 ArrayBuffer/typed array，或 9 号参数 16 字节 `ptr+size` 描述符）的
  8 字节帧头之后，再通过 N-API threadsafe function 调用
  `onWindowRasterized(windowId, cb)`。首帧早于回调注册时会补发一次通知。
- AOT：`kernel_blob.bin` 缺失时自动查找 `libapp.so` 并走
  `FlutterEngineCreateAOTData` + `FlutterProjectArgs.aot_data`；两者都存在时用
  `SKYLINE_AOT_LIBRARY` 强制 AOT。release 引擎与 `npm run app:aot` 的验证见
  [engine/README.md](engine/README.md)。
- 多窗口：每个 window 独立引擎实例与 platform thread；`destroyWindow` 停止并
  join 线程，之后可重新创建同 id 窗口。
- 诊断：`window <id> ready` / `first frame WxH` / `destroy window` /
  `window <id> stopped` 输出到 stderr；宽度/高度/dpr 非法或 bundle 缺 kernel
  时 `createWindow` 同步抛错；引擎启动有 15 s 超时。
- CMake 会自动设置 `rpath` 指向 `native/engine/out/linux-x64-debug`；引擎缺失时
  仍生成 StubEngine 版本。
- `createWindow` 运行 bundle 的默认 `main`（官方契约没有 entrypoint 参数）；需要
  `--entrypoint-arg` 的 appservice 调试路径仍使用独立 `skyline_host`。

验证：`npm run test:host-engine`（5 个用例：单窗口出帧、窗口重建、双窗口、
dpr=2 物理像素、缺失 kernel）。

## 构建

```bash
cmake -S native -B native/build
cmake --build native/build
# 产物：native/build/skyline.node
```

## 验证

```bash
SKYLINE_LINUX_SKYLINE_ADDON=native/build/skyline.node npm run test:contract
```

契约用例（L1/L2）应与 `reference/skyline-node.js` 完全一致。

## 阶段 2：Linux Flutter Engine

- `engine/` 固定 Flutter 3.35.3 公开引擎（`REVISION`、`ARTIFACTS.json`）并提供
  fetch/build 脚本；详见 [engine/README.md](engine/README.md)。
- `host/` 是 headless embedder：软件渲染一帧，写入
  `/dev/shm/skyline_<key>.dat`（16 字节外层头 + 8 字节帧头 + RGBA）。
- 引擎未构建时，CMake 仍会生成一个返回 77 的 `skyline_host` stub，
  `npm run native:build` 不受影响。

```bash
npm run toolchain:fetch   # Dart SDK + patched SDK + icudtl.dat (~250 MB)
npm run app:build         # apps/empty-page -> kernel_blob.bin
npm run engine:fetch      # 20-30 GB gclient checkout（本轮未执行）
npm run engine:build      # libflutter_engine.so + header
npm run native:build      # 用真实 host / N-API 后端替换 stub
npm run test:engine       # 校验一帧共享内存
npm run test:engine:resize  # --resize-script 更新 view metrics 后的帧尺寸
npm run test:host-engine  # skyline.node createWindow -> 共享内存帧 -> onWindowRasterized
npm run test:host-engine:aot  # release skyline.node 在 AOT-only bundle 上 createWindow
npm run test:shm-frame    # 与 skyline-shared-memory setMemory/getMemory 的帧桥接
```

符号清单：`node tools/extract-kernel-symbols.js --write`，产物为
`spec/kernel-symbols.json` 与 `docs/engine-symbols.md`。


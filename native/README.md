# Skyline Node 模块（Linux 原生骨架）

本目录实现 `skyline.node` 的 **Native N-API API 层**：

- 在模块加载时注册全局 `SkylineShell` / `SkylineDebugInfo`，方法顺序、属性描述符、
  参数校验与错误消息与官方 Windows 模块一致；
- 通过 `__skylineNative` 原生对象预留引擎边界（回调注册、窗口生命周期、事件分发）；
- 当前引擎后端为 stub：`createWindow` 明确报错，渲染尚未接入。

后续阶段用 Linux Flutter Engine 与复刻的 Skyline 运行时替换 stub，API 层与测试无需
改动。

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
npm run native:build      # 用真实 host 替换 stub
npm run test:engine       # 校验一帧共享内存
```

符号清单：`node tools/extract-kernel-symbols.js --write`，产物为
`spec/kernel-symbols.json` 与 `docs/engine-symbols.md`。


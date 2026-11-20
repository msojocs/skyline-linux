# Linux Flutter Engine (phase 2)

This directory pins the public Flutter engine used as the buildable baseline for the
native Skyline port, and contains the scripts that fetch/build it.

The official `skyline.node` was built from a **private** engine revision
(`6b917cf86ebe1b9ff148310504a736d667527e8b`, not present in any public Flutter
repository). Its 72 MB `kernel_blob.bin` was produced by Flutter 3.35.3 / Dart 3.9.2
(embedded source paths reference `D:/flutter_windows_3.35.3-stable`), so phase 2 pins
the public 3.35.3 engine instead. See `REVISION` for every revision/hash and
`ARTIFACTS.json` for download URLs.

## Quick start

```bash
# 1. Dart app toolchain (~250 MB): Dart SDK, patched SDK, icudtl.dat, header.
SKYLINE_PROXY=http://127.0.0.1:54321 npm run toolchain:fetch

# 2. Compile the minimal pure-dart:ui page into a JIT kernel blob.
npm run app:build

# 3. Engine checkout + build (20-30 GB download, 1-2 h build).
SKYLINE_PROXY=http://127.0.0.1:54321 npm run engine:fetch
npm run engine:build

# 4. Reconfigure CMake so the real host replaces the exit-77 stub.
npm run native:build

# 5. Assert one software-rendered frame reached /dev/shm.
npm run test:engine

# 5b. Assert the N-API module hosts a window and delivers a frame.
npm run test:host-engine

# 6. Phase 3: patched dart:ui + recompiled package:skyline kernel.
npm run ui:patch          # install compat layer, rebuild/stage patched SDK
npm run kernel:skyline    # reconstruct sources + compile the official app
npm run engine:build      # regenerate the core snapshot with the compat classes
npm run native:build
npm run test:kernel && npm run smoke:build && npm run test:ui-compat
```

`SKYLINE_PROXY` is a convenience for `http_proxy`/`https_proxy`; standard environment
variables work too. The scripts never run a remote build (RBE/goma are disabled).

## Status (2026-11-19)

The pinned debug engine has been fetched and built on this host:

- checkout `ddf47dd3ff96dbde6d9c614db0d7f019d7c7a2b7`, clang 18 / gn 2221 from the
  synced buildtools;
- `out/host_debug_unopt/libflutter_engine.so` (357 MB, unstripped) staged to
  `out/linux-x64-debug/` together with `flutter_embedder.h` and `icudtl.dat`;
- `npm run test:engine` passes: one 128×96 physical frame, `frameSize=49152`,
  first pixel `[255,0,0,255]`, `vsyncCount=1`, `presentCount=1`,
  shared-memory `dataSize=196616` (8 + 4 × frame).
- `npm run test:host-engine` passes with the same engine linked into
  `native/build/skyline.node`: `createWindow` starts a per-window platform
  thread, writes a 128×96 RGBA frame into the caller's data area, and invokes
  `onWindowRasterized`; multi-window, window recreation and missing-kernel
  errors are covered too (2026-11-20).

The two transient `git fetch` retry warnings during `gclient sync`
(`third_party/pkg/flutter_packages`, `third_party/ocmock`) did not affect the
headless engine build.

## Phase 2: AOT / release engine

The debug engine only executes JIT `kernel_blob.bin`.  For release, compile an
AOT ELF snapshot and build the release engine:

```bash
npm run engine:build -- --runtime-mode release   # out/linux-x64-release
npm run app:aot                                  # apps/empty-page/build/libapp.so

# Build a host against the release stage (separate build dir).
cmake -S native -B native/build-release \
  -DSKYLINE_ENGINE_ROOT="$PWD/native/engine/out/linux-x64-release"
cmake --build native/build-release --target skyline_host skyline_node

npm run test:engine:aot
npm run test:host-engine:aot
```

`build-aot.sh` recompiles the entrypoint with `frontend_server --aot --tfa`
(gen_snapshot rejects kernels compiled without TFA) and then runs
`gen_snapshot --snapshot_kind=app-aot-elf`.  The resulting bundle layout matches
Flutter:

```
<app>/build/libapp.so
<app>/build/flutter_assets/...
```

The snapshot must be produced by a gen_snapshot built with the same product
configuration as the engine.  The generic toolchain download is a JIT-release
build, so `engine:build --runtime-mode release` also builds
`out/host_release/gen_snapshot`, and `build-aot.sh` prefers it (override with
`SKYLINE_GEN_SNAPSHOT`).

The host and the N-API backend load the snapshot through
`FlutterEngineCreateAOTData` and `FlutterProjectArgs.aot_data`.  AOT is selected
automatically when `kernel_blob.bin` is absent; pass `--aot-library` (host) or
set `SKYLINE_AOT_LIBRARY` (N-API backend) to force it when both files exist.
`npm run test:host-engine:aot` builds `native/build-release/skyline.node` and
verifies `createWindow` on an AOT-only bundle (no `kernel_blob.bin`).

### Skyline appservice AOT

```bash
# Release engine with the embedded runtime exports (73 symbols + libskyline.so).
npm run runtime:embed -- --runtime-mode release

# AOT for the recompiled appservice (patched SDK).
bash native/engine/build-aot.sh \
  --app-dir native/engine/work/skyline-app \
  --entry native/engine/work/skyline-app/packages/sample/lib/devtools.dart \
  --patched-sdk

npm run test:skyline:aot   # stripes page on the release engine
```

`test:skyline:aot` starts the AOT appservice with `SKYLINE_APP_SERVICE_OPS`
pointing at `test/fixtures/appservice-stripes.ops.json` and verifies the three
stripe colors in the final frame (9.4 MB `libapp.so` on 2026-11-20).

## Phase 3: Skyline `dart:ui` compatibility layer

The official kernel and `isolate_snapshot_data` embed the private engine's
`dart:ui` sources (`Application`, `mmtrace`, `WindowStatus`, ...).  Phase 3
recovers them, installs an additive compat layer into the pinned engine, and
recompiles the official application against the public SDK:

```bash
npm run ui:patch          # dart_ui_compat/{application,mmtrace,skyline_ext}.dart
                          # + apply.py; rebuilds platform_strong.dill
npm run kernel:skyline    # tools/reconstruct-kernel-sources.js + frontend_server
npm run test:kernel       # static + compile-level assertions
npm run engine:build      # core snapshot now contains Application etc.
npm run smoke:build       # apps/dart-ui-compat-smoke against the patched SDK
npm run test:ui-compat    # runtime: Application + green frame
npm run test:appservice   # stub libskyline.so + appservice first frame
npm run runtime:embed     # embed the runtime (full Dart API) into the engine
npm run test:page         # real CreatePageRoute -> YGNode -> DecoratedBox page
```

Result (2026-11-19): kernel 80.8 MB, SDK hash `a29e08c72e`; the public engine
loads it without `Invalid SDK hash` and reaches
`package:sample/devtools.dart main`.  The stub Linux runtime
(`native/runtime/skyline_runtime_stub.cc`, 73 exports) lets the real
`initSkylineForDevtools` path run to `runApp` and present a 320×240 frame,
including the native→Dart `RunApplication` callback triggered by
`NotifyFlutterReady`:

```bash
npm run native:build
npm run test:appservice
```

Full notes and limitations:
[`docs/phase-3-dart-ui-compat.md`](../../docs/phase-3-dart-ui-compat.md).

The embedded variant (`runtime:embed`) compiles the same runtime with the full
Dart API and exports it from `libflutter_engine.so`.  It builds a real page via
`NewDecoratedBox` → `NewYGNode` → `CreatePageRoute`; `test:page` asserts a
320×240 `#FF3366` frame.

## Layout

| path | contents | committed |
| --- | --- | --- |
| `REVISION` | pinned framework/engine/Dart revisions and kernel SDK hashes | yes |
| `ARTIFACTS.json` | download URLs + SHA-256 + extraction targets | yes |
| `env.sh` | shared paths, proxy, Python 3.12 selection | yes |
| `gclient.template` | `.gclient` template with the pinned revision | yes |
| `fetch-toolchain.sh` | Dart/patched SDK/artifacts/header (~250 MB) | yes |
| `build-empty-page.sh` | compiles an app to `kernel_blob.bin` (`--patched-sdk`, `SKYLINE_APP_DIR`) | yes |
| `fetch-engine.sh` | depot_tools + `gclient sync` for the pinned engine | yes |
| `build-engine.sh` | `gn gen` + `ninja flutter_engine`, stages the host artifacts | yes |
| `dart_ui_compat/` | recovered private `dart:ui` sources + `apply.py` patches | yes |
| `patch-dart-ui.sh` | installs the compat layer, rebuilds/stages the patched SDK | yes |
| `build-skyline-kernel.sh` | reconstructs the official sources and compiles the app | yes |
| `patch-skyline-runtime.sh` | embeds the runtime in `libflutter_engine.so` (full Dart API) | yes |
| `app_compat/apply.py` | guards `YGConnector.coordinateStackingChildren` for non-stacking nodes | yes |
| `checkout/` | flutter/flutter monorepo checkout | **no** |
| `depot_tools/` | depot_tools checkout | **no** |
| `toolchain/` | downloaded SDKs, header, `icudtl.dat`, `gen_snapshot` | **no** |
| `out/linux-x64-debug/` | engine, header, `icudtl.dat`, `patched_sdk/` | **no** |
| `work/skyline-app/` | reconstructed official sources + recompiled kernel | **no** |

The engine build is a host debug/unoptimized build (`out/host_debug_unopt`), because
only a debug engine can execute `kernel_blob.bin` (JIT). `build-engine.sh
--runtime-mode release` is reserved for a later AOT step.

## Why not the prebuilt Linux engine?

The Flutter SDK ships `libflutter_linux_gtk.so`, but its embedder API symbols
(`FlutterEngineRun`, `FlutterEngineSendWindowMetricsEvent`, ...) are hidden by a
version script. The prebuilt library therefore cannot be used for a custom headless
embeddable host. `build-engine.sh` builds the `flutter_engine` target, which produces
`libflutter_engine.so` with the public embedder API.

## Symbol inventory

`tools/extract-kernel-symbols.js` parses the official bundle locally (no network):

- the concatenated kernel components, string tables and embedded
  `package:skyline` sources;
- 73 `DynamicLibrary.lookupFunction` symbols (all present in the official PE export
  table), e.g. `InitDartAPIDLSkyline`, `Register`, `YogaCalculateLayout`;
- 375 callbacks registered with `stringHash(name)` by `setupFFIFunctions`;
- FFI typedefs and `Struct` layouts;
- 275 `@Native(... symbol: '...')` entries from `isolate_snapshot_data`;
- the private `dart:ui` additions: `Application`, `createNativeParagraph()`,
  `warmUpFont()`.

Outputs: `spec/kernel-symbols.json` (machine-readable) and
`docs/engine-symbols.md` (reviewable summary). Regenerate with:

```bash
npm run spec:symbols
npm run spec:symbols:check
```

The official kernel's SDK hash is `294a28fe17`, while the public 3.35.3 patched SDK
uses `a29e08c72e`. That difference confirms the official Dart SDK is a private fork;
running the official kernel on the public engine will require stubbing the patched
`dart:ui` surface (phase 3), which is why this phase uses a separately compiled
pure-dart:ui page.

## Host contract

`native/host` builds `native/build/skyline_host` when
`out/linux-x64-debug/libflutter_engine.so` exists; otherwise a stable stub returning
exit 77 is built so `npm run native:build` keeps working.

The host uses the software renderer (`FlutterSoftwareRendererConfig`) so no EGL,
display server or GTK is needed. It sends implicit-view metrics, services
`vsync_callback` batons on the platform thread, copies the first presented frame,
converts Skia N32/BGRA to RGBA8, and writes the contract format:

```
/dev/shm/skyline_<key>.dat
  0   u64 LE  data_size (8 + 4 * frame_size, matching the official budget)
  8   i32 LE  version = 1
  12  4 bytes padding
  16  u32 LE  frame_id
  20  u32 LE  frame_size
  24  RGBA8 pixels
```

Exit codes:

| code | meaning |
| --- | --- |
| 0 | frame written |
| 70 | `FlutterEngineRun`/metrics failed |
| 71 | no frame before `--timeout-ms` |
| 72 | shared-memory write failed |
| 77 | engine artifacts missing (stub) |
| 78 | bad arguments/assets/ICU |

## Troubleshooting

| symptom | likely cause |
| --- | --- |
| host exits 77 | engine not fetched/built; rerun `engine:fetch` + `engine:build` + `native:build` |
| `Invalid kernel binary` / app never starts | engine is release/AOT; use the debug/unoptimized build |
| `Dart Error: Can't load Kernel binary: Invalid SDK hash` | the official kernel uses the private SDK hash `294a28fe17`; recompile with `npm run ui:patch && npm run kernel:skyline` (public hash `a29e08c72e`) |
| unresolved `dart:ui` members when compiling | wrong `--sdk-root` (needs trailing `/`) or wrong patched SDK |
| timeout with `vsync=0` | app did not schedule a frame; check metrics callback |
| timeout with `vsync>0` | raster thread did not present; check renderer type / surface size |
| pixel colors swapped | pass `--pixel-format rgba` (the default assumes Linux Skia N32 = BGRA) |
| `FlutterEngineRun failed: 1` | engine library version mismatch; rebuild host against the staged header |
| gclient/Python failures | ensure Python 3.12 is used (`uv` is installed by the scripts) |

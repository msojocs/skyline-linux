#!/usr/bin/env bash
# Stages a self-contained Linux Skyline addon into the WeChat DevTools tree.
#
#   bash native/engine/stage-devtools-addon.sh [--devtools-dir DIR]
#                                               [--engine-mode debug|release]
#
# The DevTools loads `node_modules/skyline-addon/build/skyline.node` from
# `resources/app.asar.unpacked`.  After this script has run, `bin/wechat-devtools`
# needs no SKYLINE_* / LD_LIBRARY_PATH variables:
#
#   * the rebuilt Dart application + assets live in `bundle/` (the official
#     kernel is compiled with the WeChat SDK and fails the rebuilt engine's
#     SDK-hash check).  `release` (default) stages the AOT snapshot
#     `bundle/libapp.so` and drops `kernel_blob.bin`; `debug` stages the JIT
#     kernel in `bundle/flutter_assets`;
#   * `libflutter_engine.so`, `libskyline.so` (symlink) and `icudtl.dat` live
#     next to `skyline.node`; both ELF files get RUNPATH=$ORIGIN so the addon
#     and Dart's `DynamicLibrary.open('libskyline.so')` resolve them in place;
#   * the addon recognizes the DevTools layout from its own path and passes the
#     `package:sample/devtools.dart` entrypoint arguments automatically;
#   * `node_modules/sharedMemory/sharedMemory.node` is replaced with the local
#     build from `native/shared-memory` (built by `npm run native:build`), which
#     returns a 16-byte handle even after the appservice page deletes
#     `process` (the stock build then hits Electron's
#     "External buffers are not allowed").
#
# `release` is the default because the debug engine compiles Skia with
# `-O0 -D_DEBUG -D_GLIBCXX_DEBUG`; its software rasterizer cannot sustain
# interactive scrolling at simulator resolutions.  The release engine runs the
# same embedded Skyline runtime and needs an AOT snapshot built with the patched
# SDK, so rebuild `bundle/libapp.so` after changing Dart runtime sources.
#
# Prerequisites (release, default):
#   bash native/engine/patch-skyline-runtime.sh --runtime-mode release
#   npm run devtools:aot        # AOT snapshot for package:sample/devtools.dart
#   cmake -S native -B native/build-release \
#     -DSKYLINE_ENGINE_ROOT="$PWD/native/engine/out/linux-x64-release" \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build native/build-release
#   npm run kernel:skyline      # assets under native/engine/work/skyline-app/build
#
# Prerequisites (debug):
#   npm run native:build        # native/build/skyline.node
#   npm run engine:build        # native/engine/out/linux-x64-debug/*
#   npm run kernel:skyline      # native/engine/work/skyline-app/build/flutter_assets
#
# Both modes also need:
#   sharedMemory.node           # built by native:build / native:build:release
#   patchelf                    # apt install patchelf / pip install patchelf
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

devtools_dir=${SKYLINE_DEVTOOLS_DIR:-"$ENGINE_REPO_ROOT/../wechat-web-devtools-linux"}
shared_memory_node=${SKYLINE_SHARED_MEMORY_NODE:-}
engine_mode=${SKYLINE_DEVTOOLS_ENGINE:-release}
while [ $# -gt 0 ]; do
  case "$1" in
    --engine-mode)
      [ $# -ge 2 ] || die "--engine-mode requires a value"
      engine_mode=$2
      shift 2
      ;;
    --engine-mode=*) engine_mode=${1#*=}; shift ;;
    --devtools-dir)
      [ $# -ge 2 ] || die "--devtools-dir requires a value"
      devtools_dir=$2
      shift 2
      ;;
    --shared-memory-node)
      [ $# -ge 2 ] || die "--shared-memory-node requires a value"
      shared_memory_node=$2
      shift 2
      ;;
    -h|--help)
      sed -n '2,49p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
done

case "$engine_mode" in
  debug)
    engine_stage="$ENGINE_DIR/out/linux-x64-debug"
    node_addon="$ENGINE_REPO_ROOT/native/build/skyline.node"
    default_shared_memory="$ENGINE_REPO_ROOT/native/build/sharedMemory.node"
    ;;
  release)
    engine_stage="$ENGINE_DIR/out/linux-x64-release"
    node_addon="$ENGINE_REPO_ROOT/native/build-release/skyline.node"
    default_shared_memory="$ENGINE_REPO_ROOT/native/build-release/sharedMemory.node"
    ;;
  *) die "unsupported engine mode: $engine_mode (expected debug or release)" ;;
esac
: "${shared_memory_node:=$default_shared_memory}"

addon_dir="$devtools_dir/resources/app.asar.unpacked/node_modules/skyline-addon"
shared_memory_dir="$devtools_dir/resources/app.asar.unpacked/node_modules/sharedMemory"
engine_lib="$engine_stage/libflutter_engine.so"
engine_icu="$engine_stage/icudtl.dat"
assets="$ENGINE_SKYLINE_APP/build/flutter_assets"
kernel="$assets/kernel_blob.bin"
aot_library="$ENGINE_SKYLINE_APP/build/libapp.so"

if [ "$engine_mode" = release ]; then
  [ -f "$node_addon" ] || die "missing $node_addon; build it with:
  cmake -S native -B native/build-release -DSKYLINE_ENGINE_ROOT=\"$engine_stage\" -DCMAKE_BUILD_TYPE=Release && cmake --build native/build-release"
  [ -f "$engine_lib" ] || die "missing $engine_lib; run: npm run runtime:embed -- --runtime-mode release"
  [ -f "$aot_library" ] || die "missing $aot_library; run: npm run devtools:aot"
else
  [ -f "$node_addon" ] || die "missing $node_addon; run: npm run native:build"
  [ -f "$engine_lib" ] || die "missing $engine_lib; run: npm run engine:build"
  [ -f "$kernel" ] || die "missing $kernel; run: npm run kernel:skyline"
fi
[ -f "$engine_icu" ] || die "missing $engine_icu; run: npm run engine:build"
[ -d "$assets" ] || die "missing $assets; run: npm run kernel:skyline"
[ -d "$addon_dir" ] || die "addon directory not found: $addon_dir"
[ -f "$shared_memory_node" ] || die \
  "missing sharedMemory addon: $shared_memory_node (run: npm run native:build or native:build:release)"
if ! nm -D --defined-only "$engine_lib" 2>/dev/null | grep -q ' SkylineApplyLiveOps$'; then
  die "$engine_lib has no embedded Skyline runtime; run:
  bash native/engine/patch-skyline-runtime.sh --runtime-mode $engine_mode"
fi

patchelf_bin=${PATCHELF:-}
if [ -z "$patchelf_bin" ]; then
  patchelf_bin=$(command -v patchelf || true)
fi
[ -n "$patchelf_bin" ] || die \
  "patchelf not found; install it (apt install patchelf, or pip install patchelf)"

log "staging addon into $addon_dir ($engine_mode engine)"

build_dir="$addon_dir/build"
bundle_dir="$addon_dir/bundle"
mkdir -p "$build_dir" "$bundle_dir"

# Native module + engine libraries.  RUNPATH=$ORIGIN lets the addon find
# libflutter_engine.so and lets the Dart VM's dlopen("libskyline.so") resolve
# the sibling symlink.
cp -f "$node_addon" "$build_dir/skyline.node"
cp -f "$engine_lib" "$build_dir/libflutter_engine.so"
ln -sfn libflutter_engine.so "$build_dir/libskyline.so"
cp -f "$engine_icu" "$build_dir/icudtl.dat"
chmod u+w "$build_dir/skyline.node" "$build_dir/libflutter_engine.so"
"$patchelf_bin" --set-rpath '$ORIGIN' "$build_dir/skyline.node"
"$patchelf_bin" --set-rpath '$ORIGIN' "$build_dir/libflutter_engine.so"

# Rebuilt application (public SDK + dart:ui compat layer) and its non-VM
# assets.  The official kernel in the DevTools bundle is rejected by the
# rebuilt engine with `Can't load Kernel binary: Invalid SDK hash`.
if [ -d "$bundle_dir/flutter_assets" ]; then
  rm -rf "$bundle_dir/flutter_assets"
fi
cp -a "$assets" "$bundle_dir/flutter_assets"
cp -f "$engine_icu" "$bundle_dir/icudtl.dat"
if [ "$engine_mode" = release ]; then
  # A release engine cannot read a JIT kernel.  Its presence would also make
  # host_engine.cc pick the kernel path, so keep only the AOT snapshot.
  rm -f "$bundle_dir/flutter_assets/kernel_blob.bin"
  cp -f "$aot_library" "$bundle_dir/libapp.so"
else
  rm -f "$bundle_dir/libapp.so"
fi

# The stock Linux sharedMemory addon detects Electron via `process.versions`,
# but the appservice page deletes `process`; setMemory then tries to return an
# external ArrayBuffer, which Electron rejects ("External buffers are not
# allowed").  The in-repo native/shared-memory build recognizes Electron from
# the user agent as well and returns the 16-byte handle.
[ -d "$shared_memory_dir" ] || die \
  "sharedMemory directory not found: $shared_memory_dir"
cp -f "$shared_memory_node" "$shared_memory_dir/sharedMemory.node"

# Verify the staged files are what the addon will actually load.
if [ "$engine_mode" = release ]; then
  [ -f "$bundle_dir/libapp.so" ] || die "libapp.so missing after staging"
  [ ! -f "$bundle_dir/flutter_assets/kernel_blob.bin" ] ||
    die "release staging still contains kernel_blob.bin"
else
  [ -f "$bundle_dir/flutter_assets/kernel_blob.bin" ] ||
    die "kernel_blob.bin missing after staging"
fi
[ -f "$bundle_dir/flutter_assets/FontManifest.json" ] ||
  die "flutter_assets is incomplete after staging"
readelf -d "$build_dir/skyline.node" | grep -q 'RUNPATH.*\$ORIGIN' ||
  die "skyline.node RUNPATH was not set"
readelf -d "$build_dir/libflutter_engine.so" | grep -q 'RUNPATH.*\$ORIGIN' ||
  die "libflutter_engine.so RUNPATH was not set"

log "staged:"
du -sh "$build_dir/skyline.node" "$build_dir/libflutter_engine.so" \
  "$bundle_dir/flutter_assets" "$shared_memory_dir/sharedMemory.node" >&2
if [ "$engine_mode" = release ]; then
  du -sh "$bundle_dir/libapp.so" >&2
fi
log "start with: $devtools_dir/bin/wechat-devtools"

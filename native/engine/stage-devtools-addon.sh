#!/usr/bin/env bash
# Stages a self-contained Linux Skyline addon into the WeChat DevTools tree.
#
#   bash native/engine/stage-devtools-addon.sh [--devtools-dir DIR]
#
# The DevTools loads `node_modules/skyline-addon/build/skyline.node` from
# `resources/app.asar.unpacked`.  After this script has run, `bin/wechat-devtools`
# needs no SKYLINE_* / LD_LIBRARY_PATH variables:
#
#   * the rebuilt Dart kernel + assets live in `bundle/flutter_assets` (the
#     official kernel is compiled with the WeChat SDK and fails the rebuilt
#     engine's SDK-hash check);
#   * `libflutter_engine.so`, `libskyline.so` (symlink) and `icudtl.dat` live
#     next to `skyline.node`; both ELF files get RUNPATH=$ORIGIN so the addon
#     and Dart's `DynamicLibrary.open('libskyline.so')` resolve them in place;
#   * the addon recognizes the DevTools layout from its own path and passes the
#     `package:sample/devtools.dart` entrypoint arguments automatically;
#   * `node_modules/sharedMemory/sharedMemory.node` is replaced with the local
#     build, which returns a 16-byte handle even after the appservice page
#     deletes `process` (the stock build then hits Electron's
#     "External buffers are not allowed").
#
# Prerequisites:
#   npm run native:build        # native/build/skyline.node
#   npm run engine:build        # native/engine/out/linux-x64-debug/*
#   npm run kernel:skyline      # native/engine/work/skyline-app/build/flutter_assets
#   (cd ../skyline-shared-memory/build && ninja)   # patched sharedMemory.node
#   patchelf                    # apt install patchelf / pip install patchelf
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

devtools_dir=${SKYLINE_DEVTOOLS_DIR:-"$ENGINE_REPO_ROOT/../wechat-web-devtools-linux"}
shared_memory_node=${SKYLINE_SHARED_MEMORY_NODE:-"$ENGINE_REPO_ROOT/../skyline-shared-memory/build/Release/sharedMemory.node"}
while [ $# -gt 0 ]; do
  case "$1" in
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
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
done

addon_dir="$devtools_dir/resources/app.asar.unpacked/node_modules/skyline-addon"
shared_memory_dir="$devtools_dir/resources/app.asar.unpacked/node_modules/sharedMemory"
node_addon="$ENGINE_REPO_ROOT/native/build/skyline.node"
engine_lib="$ENGINE_STAGE/libflutter_engine.so"
engine_icu="$ENGINE_STAGE/icudtl.dat"
assets="$ENGINE_SKYLINE_APP/build/flutter_assets"
kernel="$assets/kernel_blob.bin"

[ -f "$node_addon" ] || die "missing $node_addon; run: npm run native:build"
[ -f "$engine_lib" ] || die "missing $engine_lib; run: npm run engine:build"
[ -f "$engine_icu" ] || die "missing $engine_icu; run: npm run engine:build"
[ -f "$kernel" ] || die "missing $kernel; run: npm run kernel:skyline"
[ -d "$addon_dir" ] || die "addon directory not found: $addon_dir"
[ -f "$shared_memory_node" ] || die \
  "missing patched sharedMemory addon: $shared_memory_node (build ../skyline-shared-memory)"

patchelf_bin=${PATCHELF:-}
if [ -z "$patchelf_bin" ]; then
  patchelf_bin=$(command -v patchelf || true)
fi
[ -n "$patchelf_bin" ] || die \
  "patchelf not found; install it (apt install patchelf, or pip install patchelf)"

log "staging addon into $addon_dir"

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

# Rebuilt kernel (public SDK + dart:ui compat layer) and its non-VM assets.
# The official kernel in the DevTools bundle is rejected by the rebuilt engine
# with `Can't load Kernel binary: Invalid SDK hash`.
if [ -d "$bundle_dir/flutter_assets" ]; then
  rm -rf "$bundle_dir/flutter_assets"
fi
cp -a "$assets" "$bundle_dir/flutter_assets"
cp -f "$engine_icu" "$bundle_dir/icudtl.dat"

# The stock Linux sharedMemory addon detects Electron via `process.versions`,
# but the appservice page deletes `process`; setMemory then tries to return an
# external ArrayBuffer, which Electron rejects ("External buffers are not
# allowed").  The sibling skyline-shared-memory build recognizes Electron from
# the user agent as well and returns the 16-byte handle.
[ -d "$shared_memory_dir" ] || die \
  "sharedMemory directory not found: $shared_memory_dir"
cp -f "$shared_memory_node" "$shared_memory_dir/sharedMemory.node"

# Verify the staged files are what the addon will actually load.
[ -f "$bundle_dir/flutter_assets/kernel_blob.bin" ] ||
  die "kernel_blob.bin missing after staging"
readelf -d "$build_dir/skyline.node" | grep -q 'RUNPATH.*\$ORIGIN' ||
  die "skyline.node RUNPATH was not set"
readelf -d "$build_dir/libflutter_engine.so" | grep -q 'RUNPATH.*\$ORIGIN' ||
  die "libflutter_engine.so RUNPATH was not set"

log "staged:"
du -sh "$build_dir/skyline.node" "$build_dir/libflutter_engine.so" \
  "$bundle_dir/flutter_assets" "$shared_memory_dir/sharedMemory.node" >&2
log "start with: $devtools_dir/bin/wechat-devtools"

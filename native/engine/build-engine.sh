#!/usr/bin/env bash
# Builds libflutter_engine.so + flutter_embedder.h for the Linux host and stages them
# under native/engine/out/<platform>-<mode>/.
#
#   bash native/engine/build-engine.sh [--check] [--runtime-mode debug|release]
#
# Requires native/engine/fetch-engine.sh to have completed.  Debug/unoptimized is the
# only mode that can execute kernel_blob.bin (JIT); release is for a later AOT step.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

check_only=0
runtime_mode=debug
while [ $# -gt 0 ]; do
  case "$1" in
    --check) check_only=1 ;;
    --runtime-mode)
      [ $# -ge 2 ] || die "--runtime-mode needs a value"
      runtime_mode=$2
      shift
      ;;
    --runtime-mode=*) runtime_mode=${1#*=} ;;
    -h|--help)
      sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

case "$runtime_mode" in
  debug)
    gn_mode_args=(--unoptimized --runtime-mode=debug)
    gn_out="host_debug_unopt"
    stage="$ENGINE_DIR/out/linux-x64-debug"
    ;;
  release)
    gn_mode_args=(--runtime-mode=release)
    gn_out="host_release"
    stage="$ENGINE_DIR/out/linux-x64-release"
    ;;
  *) die "unsupported runtime mode: $runtime_mode" ;;
esac

engine_root="$ENGINE_CHECKOUT/engine/src"
engine_src="$engine_root/flutter"
out_dir="$engine_root/out/$gn_out"

if [ "$check_only" -eq 1 ]; then
  for f in "$stage/libflutter_engine.so" "$stage/flutter_embedder.h"; do
    [ -e "$f" ] && log "ok: $f" || { log "missing: $f"; exit 1; }
  done
  exit 0
fi

[ -d "$engine_src" ] || die "engine checkout missing; run native/engine/fetch-engine.sh"
require_cmd ninja
ensure_python312
export PATH="$ENGINE_TOOLCHAIN/bin:$ENGINE_DEPOT_TOOLS:$PATH"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_PYTHON3="$SKYLINE_PYTHON"
unset GOMA_DIR GOMA_USE_LOCAL_SANDBOX 2>/dev/null || true

cd "$engine_src"
log "gn gen ($runtime_mode)"
"$SKYLINE_PYTHON" tools/gn "${gn_mode_args[@]}" --no-rbe --no-goma

if grep -Eq '^use_rbe = true$' "$out_dir/args.gn" 2>/dev/null; then
  die "RBE unexpectedly enabled in $out_dir/args.gn"
fi

log "ninja -C $out_dir flutter_engine"
ninja -C "$out_dir" flutter_engine

if [ "$runtime_mode" = "release" ]; then
  # AOT snapshots must match the engine's product VM configuration; the
  # downloaded toolchain gen_snapshot is a JIT-release build.
  log "ninja -C $out_dir gen_snapshot"
  ninja -C "$out_dir" gen_snapshot
fi

lib="$out_dir/libflutter_engine.so"
header="$out_dir/flutter_embedder.h"
[ -f "$lib" ] || die "build did not produce $lib"
[ -f "$header" ] || die "build did not produce $header"

mkdir -p "$stage"
cp -f "$lib" "$stage/libflutter_engine.so"
cp -f "$header" "$stage/flutter_embedder.h"

# The engine checkout ships the matching ICU data; fall back to the toolchain copy.
icu_candidates=(
  "$out_dir/icudtl.dat"
  "$engine_src/third_party/icu/flutter/icudtl.dat"
  "$ENGINE_TOOLCHAIN/engine-artifacts/icudtl.dat"
)
for candidate in "${icu_candidates[@]}"; do
  if [ -f "$candidate" ]; then
    cp -f "$candidate" "$stage/icudtl.dat"
    break
  fi
done
[ -f "$stage/icudtl.dat" ] || warn "icudtl.dat not found; the host will need --icu"

nm -D --defined-only "$stage/libflutter_engine.so" | grep -q ' FlutterEngineRun$' \
  || die "libflutter_engine.so does not export FlutterEngineRun"
nm -D --defined-only "$stage/libflutter_engine.so" | grep -q ' FlutterEngineSendWindowMetricsEvent$' \
  || die "libflutter_engine.so does not export FlutterEngineSendWindowMetricsEvent"

log "staged engine in $stage"
ls -l "$stage"
log "next: npm run native:build && npm run toolchain:fetch && npm run app:build && npm run test:engine"

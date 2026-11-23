#!/usr/bin/env bash
# Installs the Skyline `dart:ui` compatibility layer into the pinned engine
# checkout and rebuilds the patched SDK (and optionally the engine).
#
#   bash native/engine/patch-dart-ui.sh [--check] [--engine]
#
# The layer is additive: it adds new parts (`application.dart`, `mmtrace.dart`,
# `skyline_ext.dart`) and a small block of static PlatformDispatcher helpers.
# It deliberately does not take the private engine's changes to the
# embedder-facing hooks (`_addView`, `render`, `updateSemantics`, ...), whose
# signatures must match the public C++ engine.
#
# --check   verify the compat layer is present without modifying anything
# --engine  also rebuild `flutter_engine` (regenerates the core snapshots)
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

ui_dir="$ENGINE_CHECKOUT/engine/src/flutter/lib/ui"
engine_src="$ENGINE_CHECKOUT/engine/src"
out_dir="$ENGINE_BUILD_DIR"
compat_dir="$SCRIPT_DIR/dart_ui_compat"
patched_sdk_dir="$ENGINE_STAGE/patched_sdk"

check_only=0
build_engine=0
for arg in "$@"; do
  case "$arg" in
    --check) check_only=1 ;;
    --engine) build_engine=1 ;;
    -h|--help)
      sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
done

[ -d "$ui_dir" ] || die "missing engine checkout: $ui_dir (run npm run engine:fetch)"

if [ "$check_only" -eq 1 ]; then
  for part in application.dart mmtrace.dart skyline_ext.dart; do
    [ -f "$ui_dir/$part" ] || die "dart:ui compat part missing: $ui_dir/$part"
    grep -q "part '$part';" "$ui_dir/ui.dart" || die "ui.dart does not declare $part"
  done
  grep -q "static bool isUseParallelPipeline()" "$ui_dir/platform_dispatcher.dart" \
    || die "platform_dispatcher.dart lacks isUseParallelPipeline"
  grep -q "result\[0\] |= 1 << 22;" "$ui_dir/text.dart" \
    || die "text.dart lacks the private text flag encoding"
  grep -q "kTSLineHeightIndex" "$ui_dir/text/paragraph_builder.cc" \
    || die "paragraph_builder.cc lacks the private text flag decoding"
  grep -q "background_use_line_metrics" "$engine_src/flutter/txt/src/txt/paragraph_style.h" \
    || die "txt paragraph_style.h lacks backgroundUseLineMetrics"
  grep -q "getBackgroundUseLineMetrics()" \
    "$engine_src/flutter/third_party/skia/modules/skparagraph/src/TextLine.cpp" \
    || die "SkParagraph TextLine.cpp lacks backgroundUseLineMetrics"
  grep -q "matchFamilyStyle(familyName.c_str(), fontStyle)" \
    "$engine_src/flutter/third_party/skia/modules/skparagraph/src/FontCollection.cpp" \
    || die "SkParagraph FontCollection.cpp lacks primary color emoji fallback"
  grep -q "FcPatternGetWithBinding(pattern, object, id" \
    "$engine_src/flutter/third_party/skia/src/ports/SkFontMgr_fontconfig.cpp" \
    || die "SkFontMgr_fontconfig.cpp lacks the weak-binding lookup"
  log "dart:ui compat layer is installed"
  exit 0
fi

for part in application.dart mmtrace.dart skyline_ext.dart; do
  cp "$compat_dir/$part" "$ui_dir/$part"
done

python3 "$compat_dir/apply.py" "$ENGINE_CHECKOUT/engine/src"

log "dart:ui compat layer installed in $ui_dir"

if [ ! -d "$out_dir" ]; then
  warn "engine build directory missing: $out_dir (run npm run engine:build first)"
  exit 0
fi

require_cmd ninja
ensure_python312
export PATH="$ENGINE_TOOLCHAIN/bin:$ENGINE_DEPOT_TOOLS:$PATH"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_PYTHON3="$SKYLINE_PYTHON"
unset GOMA_DIR GOMA_USE_LOCAL_SANDBOX 2>/dev/null || true

log "rebuilding patched SDK"
ninja -C "$out_dir" flutter_patched_sdk/platform_strong.dill

mkdir -p "$patched_sdk_dir"
cp "$out_dir/flutter_patched_sdk/platform_strong.dill" "$patched_sdk_dir/"
cp "$out_dir/flutter_patched_sdk/vm_outline_strong.dill" "$patched_sdk_dir/"
log "patched SDK staged in $patched_sdk_dir"

if [ "$build_engine" -eq 1 ]; then
  log "rebuilding flutter_engine"
  ninja -C "$out_dir" flutter_engine
fi

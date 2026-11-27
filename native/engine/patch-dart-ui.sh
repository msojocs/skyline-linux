#!/usr/bin/env bash
# Installs the Skyline `dart:ui` compatibility layer into the pinned engine
# checkout and rebuilds the patched SDK (and optionally the engine).
#
#   bash native/engine/patch-dart-ui.sh [--check] [--engine]
#
# The layer is additive: it copies three new `dart:ui` parts (`application.dart`,
# `mmtrace.dart`, `skyline_ext.dart`) and applies unified diff patches from
# `native/engine/patches/` to the pinned engine sources.  It deliberately does
# not take the private engine's changes to the embedder-facing hooks
# (`_addView`, `render`, `updateSemantics`, ...), whose signatures must match the
# public C++ engine.
#
# Patches are grouped by the repository that owns the files:
#   dart-ui-compat.patch    flutter/lib/ui/*.dart + text/paragraph_builder.*
#   txt-text-flags.patch    flutter/txt/src/** private text flags
#   skia-text.patch         third_party/skia skparagraph + fontconfig
#   dart-io-platform.patch  third_party/dart sdk/lib/io/platform.dart
#
# --check   verify the compat layer is present without modifying anything
# --engine  also rebuild `flutter_engine` (regenerates the core snapshots)
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

ui_dir="$ENGINE_CHECKOUT/engine/src/flutter/lib/ui"
out_dir="$ENGINE_BUILD_DIR"
compat_dir="$SCRIPT_DIR/dart_ui_compat"
patches_dir="$SCRIPT_DIR/patches"
patched_sdk_dir="$ENGINE_STAGE/patched_sdk"

# `repo|patch` pairs.  The gclient root is the git worktree that tracks
# `engine/src/flutter/lib/ui` and `flutter/txt`; skia and dart are nested
# repositories of their own, so each patch applies from its own root.
patch_specs=(
  "$ENGINE_CHECKOUT|$patches_dir/dart-ui-compat.patch"
  "$ENGINE_CHECKOUT|$patches_dir/txt-text-flags.patch"
  "$ENGINE_CHECKOUT/engine/src/flutter/third_party/skia|$patches_dir/skia-text.patch"
  "$ENGINE_CHECKOUT/engine/src/flutter/third_party/dart|$patches_dir/dart-io-platform.patch"
)

check_only=0
build_engine=0
for arg in "$@"; do
  case "$arg" in
    --check) check_only=1 ;;
    --engine) build_engine=1 ;;
    -h|--help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
done

[ -d "$ui_dir" ] || die "missing engine checkout: $ui_dir (run npm run engine:fetch)"
[ -d "$patches_dir" ] || die "missing patch directory: $patches_dir"

for spec in "${patch_specs[@]}"; do
  repo=${spec%%|*}
  patch=${spec#*|}
  [ -f "$patch" ] || die "missing compat patch: $patch"
  git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "not a git worktree: $repo (run npm run engine:fetch)"
done

compat_parts=(application.dart mmtrace.dart skyline_ext.dart)

check_parts() {
  local part
  for part in "${compat_parts[@]}"; do
    [ -f "$ui_dir/$part" ] || die "dart:ui compat part missing: $ui_dir/$part (run without --check)"
    cmp -s "$compat_dir/$part" "$ui_dir/$part" ||
      die "dart:ui compat part is stale: $ui_dir/$part (run without --check)"
    grep -q "part '$part';" "$ui_dir/ui.dart" || die "ui.dart does not declare $part"
  done
}

# The parts are new files, so they are copied rather than patched.  Copy only
# when the content differs to keep ninja's view of them stable.
install_parts() {
  local part
  for part in "${compat_parts[@]}"; do
    [ -f "$compat_dir/$part" ] || die "missing compat part: $compat_dir/$part"
    if ! cmp -s "$compat_dir/$part" "$ui_dir/$part"; then
      cp "$compat_dir/$part" "$ui_dir/$part"
      log "installed: $part"
    fi
  done
}

patch_is_applied() {
  git -C "$1" apply --reverse --check "$2" >/dev/null 2>&1
}

if [ "$check_only" -eq 1 ]; then
  check_parts
  for spec in "${patch_specs[@]}"; do
    repo=${spec%%|*}
    patch=${spec#*|}
    patch_is_applied "$repo" "$patch" ||
      die "compat patch not applied: $(basename "$patch") (run without --check)"
  done
  log "dart:ui compat layer is installed"
  exit 0
fi

install_parts

for spec in "${patch_specs[@]}"; do
  repo=${spec%%|*}
  patch=${spec#*|}
  name=$(basename "$patch")
  if patch_is_applied "$repo" "$patch"; then
    log "already applied: $name"
    continue
  fi
  if ! git -C "$repo" apply --check "$patch" 2>/dev/null; then
    die "$name does not apply to $repo; the checkout is neither clean nor patched (re-run npm run engine:fetch or revert the engine sources)"
  fi
  git -C "$repo" apply "$patch"
  log "applied: $name"
done

check_parts
log "dart:ui compat layer installed in $ui_dir"

if [ ! -d "$out_dir" ]; then
  # Exiting 0 here would leave a previously staged patched SDK in place, and
  # kernel:skyline would silently compile against the old dart:ui.
  die "engine build directory missing: $out_dir; the compat sources are patched but the patched SDK was not rebuilt. Run npm run engine:build, then npm run ui:patch again"
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

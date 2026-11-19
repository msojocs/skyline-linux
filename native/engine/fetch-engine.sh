#!/usr/bin/env bash
# Fetches the pinned Flutter engine checkout (monorepo) with depot_tools/gclient.
#
#   bash native/engine/fetch-engine.sh [--check] [--full-history]
#
# This is the multi-GiB step.  It is idempotent: rerunning resumes the sync.
# Proxy: SKYLINE_PROXY=http://127.0.0.1:54321 bash native/engine/fetch-engine.sh
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

check_only=0
full_history=0
for arg in "$@"; do
  case "$arg" in
    --check) check_only=1 ;;
    --full-history) full_history=1 ;;
    -h|--help)
      sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
done

rev=$(revision_value FLUTTER_GIT_REV)
depot_rev=$(revision_value DEPOT_TOOLS_REV)
[ -n "$rev" ] || die "FLUTTER_GIT_REV missing from $ENGINE_REVISION_FILE"

check_state() {
  local ok=0
  if [ ! -d "$ENGINE_CHECKOUT/.git" ]; then
    log "checkout: missing ($ENGINE_CHECKOUT)"
    ok=1
  else
    local got
    got=$(git -C "$ENGINE_CHECKOUT" rev-parse HEAD 2>/dev/null || echo unknown)
    if [ "$got" != "$rev" ]; then
      log "checkout: revision mismatch (have $got, want $rev)"
      ok=1
    else
      log "checkout: $rev"
    fi
  fi
  for path in \
    "engine/src/flutter/third_party/gn/gn" \
    "engine/src/flutter/buildtools/linux-x64/clang/bin/clang++" \
    "engine/src/flutter/third_party/dart" \
    "engine/src/flutter/third_party/icu"; do
    if [ -e "$ENGINE_CHECKOUT/$path" ]; then
      log "deps: $path"
    else
      log "deps: missing $path"
      ok=1
    fi
  done
  if [ -f "$ENGINE_STAGE/libflutter_engine.so" ]; then
    log "stage: $ENGINE_STAGE/libflutter_engine.so"
  else
    log "stage: not built yet (run build-engine.sh)"
  fi
  return "$ok"
}

if [ "$check_only" -eq 1 ]; then
  check_state || exit 1
  exit 0
fi

require_cmd git
require_cmd curl
ensure_python312
check_disk 30 "$ENGINE_DIR"

# depot_tools at the revision pinned by the engine's DEPS.
if [ ! -d "$ENGINE_DEPOT_TOOLS/.git" ]; then
  log "cloning depot_tools"
  rm -rf "$ENGINE_DEPOT_TOOLS"
  git clone --filter=blob:none https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "$ENGINE_DEPOT_TOOLS"
fi
if [ -n "$depot_rev" ]; then
  current_depot=$(git -C "$ENGINE_DEPOT_TOOLS" rev-parse HEAD 2>/dev/null || echo unknown)
  if [ "$current_depot" != "$depot_rev" ]; then
    log "checking out depot_tools $depot_rev"
    git -C "$ENGINE_DEPOT_TOOLS" fetch origin "$depot_rev" 2>/dev/null \
      || git -C "$ENGINE_DEPOT_TOOLS" fetch origin
    git -C "$ENGINE_DEPOT_TOOLS" checkout -q "$depot_rev"
  fi
fi

export PATH="$ENGINE_TOOLCHAIN/bin:$ENGINE_DEPOT_TOOLS:$PATH"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_PYTHON3="$SKYLINE_PYTHON"
export GCLIENT_PY3=1

mkdir -p "$ENGINE_CHECKOUT"
sed "s/@FLUTTER_GIT_REV@/$rev/g" "$ENGINE_DIR/gclient.template" > "$ENGINE_CHECKOUT/.gclient"

cd "$ENGINE_CHECKOUT"
base_sync_args=(-D -r "$rev")

if [ ! -d .git ]; then
  log "first gclient sync into $ENGINE_CHECKOUT (this downloads many GiB)"
else
  got=$(git rev-parse HEAD 2>/dev/null || echo unknown)
  log "gclient sync (current $got, target $rev)"
fi

if [ "$full_history" -eq 0 ]; then
  if ! gclient sync "${base_sync_args[@]}" --no-history; then
    warn "gclient sync --no-history failed; retrying with full history"
    gclient sync "${base_sync_args[@]}"
  fi
else
  gclient sync "${base_sync_args[@]}"
fi

got=$(git rev-parse HEAD)
[ "$got" = "$rev" ] || die "revision mismatch after sync: $got != $rev"

# gclient sync runs hooks by default; this is a no-op safety net for resumed runs.
gclient runhooks

check_state || die "checkout is incomplete after sync"
log "engine checkout ready: $ENGINE_CHECKOUT"
log "next: bash native/engine/build-engine.sh"

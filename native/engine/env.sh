#!/usr/bin/env bash
# Shared configuration for native/engine/*.sh.  Source this file; do not execute it.
#
# Paths can be overridden for CI or a pre-existing checkout:
#   SKYLINE_ENGINE_CHECKOUT=/mnt/build/flutter
#   SKYLINE_DEPOT_TOOLS=/opt/depot_tools
#   SKYLINE_ENGINE_STAGE=/mnt/build/engine-out
#   SKYLINE_PROXY=http://127.0.0.1:54321

ENGINE_REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ENGINE_DIR="$ENGINE_REPO_ROOT/native/engine"
ENGINE_REVISION_FILE="$ENGINE_DIR/REVISION"
ENGINE_CHECKOUT=${SKYLINE_ENGINE_CHECKOUT:-"$ENGINE_DIR/checkout"}
ENGINE_DEPOT_TOOLS=${SKYLINE_DEPOT_TOOLS:-"$ENGINE_DIR/depot_tools"}
ENGINE_TOOLCHAIN=${SKYLINE_ENGINE_TOOLCHAIN:-"$ENGINE_DIR/toolchain"}
ENGINE_DOWNLOADS="$ENGINE_TOOLCHAIN/downloads"
ENGINE_STAGE=${SKYLINE_ENGINE_STAGE:-"$ENGINE_DIR/out/linux-x64-debug"}
# GN's SRC_ROOT is engine/src, so build outputs land in engine/src/out/... even
# though tools/gn lives under engine/src/flutter/tools.
ENGINE_BUILD_DIR="$ENGINE_CHECKOUT/engine/src/out/host_debug_unopt"
if [ -n "${SKYLINE_APP_DIR:-}" ]; then
  case "$SKYLINE_APP_DIR" in
    /*) ENGINE_APP_DIR="$SKYLINE_APP_DIR" ;;
    *) ENGINE_APP_DIR="$ENGINE_REPO_ROOT/$SKYLINE_APP_DIR" ;;
  esac
else
  ENGINE_APP_DIR="$ENGINE_REPO_ROOT/apps/empty-page"
fi
ENGINE_APP_ASSETS="$ENGINE_APP_DIR/build/flutter_assets"

if [ -n "${SKYLINE_PROXY:-}" ]; then
  export http_proxy="$SKYLINE_PROXY"
  export https_proxy="$SKYLINE_PROXY"
  export HTTP_PROXY="$SKYLINE_PROXY"
  export HTTPS_PROXY="$SKYLINE_PROXY"
  export ALL_PROXY="$SKYLINE_PROXY"
fi

log()  { printf '[engine] %s\n' "$*" >&2; }
warn() { printf '[engine] warning: %s\n' "$*" >&2; }
die()  { printf '[engine] error: %s\n' "$*" >&2; exit 1; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

# Reads a KEY=VALUE entry from REVISION without executing it as shell code.
revision_value() {
  local key=$1
  sed -n "s/^${key}=//p" "$ENGINE_REVISION_FILE" | head -n 1
}

# Prints the value of a JSON string field from ARTIFACTS.json using node.
artifact_value() {
  local path=$1
  node -e '
    const fs = require("fs");
    const obj = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
    const parts = process.argv[2].split(".");
    let v = obj;
    for (const p of parts) v = v && v[p];
    process.stdout.write(v == null ? "" : String(v));
  ' "$ENGINE_DIR/ARTIFACTS.json" "$path"
}

# Prefer Python 3.12 (engine tooling does not support the host's 3.14 yet).
# Uses uv when available; otherwise falls back to the system python3 with a warning.
ensure_python312() {
  if [ -n "${SKYLINE_PYTHON:-}" ] && [ -x "$SKYLINE_PYTHON" ]; then
    return 0
  fi
  if command -v uv >/dev/null 2>&1; then
    uv python install 3.12 >/dev/null 2>&1 || warn "uv python install 3.12 failed; trying uv python find"
    local py
    py=$(uv python find 3.12 2>/dev/null || true)
    if [ -n "$py" ] && [ -x "$py" ]; then
      mkdir -p "$ENGINE_TOOLCHAIN/bin"
      ln -sfn "$py" "$ENGINE_TOOLCHAIN/bin/python3"
      export PATH="$ENGINE_TOOLCHAIN/bin:$PATH"
      export SKYLINE_PYTHON="$py"
      log "using python 3.12: $py"
      return 0
    fi
  fi
  local sys
  sys=$(command -v python3 || true)
  [ -n "$sys" ] || die "python3 is required (install uv or python3.12)"
  local ver
  ver=$("$sys" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null || echo unknown)
  if [ "$ver" != "3.12" ]; then
    warn "engine tooling is validated with Python 3.12; found $ver at $sys"
  fi
  export SKYLINE_PYTHON="$sys"
}

disk_free_gib() {
  df -Pk "$1" 2>/dev/null | awk 'NR==2 {printf "%d", $4/1024/1024}'
}

check_disk() {
  local need=$1 dir=$2
  local avail
  avail=$(disk_free_gib "$dir")
  if [ -z "$avail" ] || [ "$avail" -lt "$need" ]; then
    die "need about ${need} GiB free under $dir (available: ${avail:-unknown} GiB)"
  fi
}

# Phase 3: reconstructed official sources and the compat-patched SDK.
ENGINE_WORK_DIR=${SKYLINE_ENGINE_WORK:-"$ENGINE_DIR/work"}
ENGINE_SKYLINE_APP="$ENGINE_WORK_DIR/skyline-app"
ENGINE_PATCHED_SDK=${SKYLINE_PATCHED_SDK:-"$ENGINE_STAGE/patched_sdk"}

#!/usr/bin/env bash
# Compiles a pure dart:ui app into a JIT kernel blob for the debug engine.
#
#   bash native/engine/build-empty-page.sh [--verbose] [--patched-sdk]
#
#   SKYLINE_APP_DIR=apps/my-app   pick a different app (default apps/empty-page)
#   --patched-sdk                 compile against the Skyline compat SDK
#                                 (native/engine/out/linux-x64-debug/patched_sdk)
#
# Requires native/engine/fetch-toolchain.sh to have completed.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

verbose=0
use_patched_sdk=0
for arg in "$@"; do
  case "$arg" in
    --verbose) verbose=1 ;;
    --patched-sdk) use_patched_sdk=1 ;;
    -h|--help)
      sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
done

dartaotruntime="$ENGINE_TOOLCHAIN/dart-sdk/bin/dartaotruntime"
frontend="$ENGINE_TOOLCHAIN/engine-artifacts/frontend_server_aot.dart.snapshot"
if [ "$use_patched_sdk" -eq 1 ]; then
  patched_sdk="$ENGINE_PATCHED_SDK"
else
  patched_sdk="$ENGINE_TOOLCHAIN/flutter_patched_sdk"
fi
entry="$ENGINE_APP_DIR/lib/main.dart"
packages="$ENGINE_APP_DIR/package_config.json"
out="$ENGINE_APP_ASSETS/kernel_blob.bin"

for f in "$dartaotruntime" "$frontend" "$patched_sdk/platform_strong.dill" "$entry" "$packages"; do
  [ -e "$f" ] || die "missing $f; run: npm run toolchain:fetch"
done

mkdir -p "$ENGINE_APP_ASSETS"
rm -f "$out"

args=(
  --sdk-root "$patched_sdk/"
  --target=flutter
  --no-print-incremental-dependencies
  --packages "$packages"
  --output-dill "$out"
)
[ "$verbose" -eq 1 ] && args+=(--verbose)

log "compiling $entry"
"$dartaotruntime" "$frontend" "${args[@]}" "$entry"

[ -s "$out" ] || die "frontend_server produced no kernel blob"
size=$(wc -c < "$out")
sha=$(sha256sum "$out" | awk '{print $1}')
log "kernel_blob.bin: $size bytes, sha256 $sha"

expected_hash=$(revision_value PUBLIC_KERNEL_SDK_HASH)
if [ -n "$expected_hash" ] && ! head -c 64 "$out" | grep -aq "$expected_hash"; then
  warn "kernel header does not contain SDK hash $expected_hash; check the pinned frontend_server/patched SDK"
fi

log "assets ready: $ENGINE_APP_ASSETS"

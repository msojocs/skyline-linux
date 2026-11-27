#!/usr/bin/env bash
# Compiles a Dart application into an AOT ELF snapshot (`libapp.so`) for the
# release Flutter engine, using the pinned Dart SDK + gen_snapshot.
#
#   bash native/engine/build-aot.sh [--app-dir DIR] [--entry FILE]
#                                   [--target-os linux] [--patched-sdk]
#
# The bundle layout matches the official one:
#
#   <app>/build/libapp.so                 <- AOT snapshot
#   <app>/build/flutter_assets/...        <- assets + optional JIT kernel
#
# `gen_snapshot` requires a kernel compiled with TFA, so this script recompiles
# the entrypoint with `frontend_server --aot --tfa` instead of reusing the JIT
# kernel_blob.bin.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

app_dir="$ENGINE_APP_DIR"
entry=""
target_os=linux
patched_sdk=0
while [ $# -gt 0 ]; do
  arg=$1
  case "$arg" in
    --app-dir)
      [ $# -ge 2 ] || die "--app-dir needs a value"
      app_dir=$2
      shift
      ;;
    --app-dir=*) app_dir=${arg#*=} ;;
    --entry)
      [ $# -ge 2 ] || die "--entry needs a value"
      entry=$2
      shift
      ;;
    --entry=*) entry=${arg#*=} ;;
    --target-os)
      [ $# -ge 2 ] || die "--target-os needs a value"
      target_os=$2
      shift
      ;;
    --target-os=*) target_os=${arg#*=} ;;
    --patched-sdk) patched_sdk=1 ;;
    -h|--help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
  shift
done

case "$app_dir" in
  /*) ;;
  *) app_dir="$ENGINE_REPO_ROOT/$app_dir" ;;
esac
if [ -z "$entry" ]; then
  if [ -f "$app_dir/lib/main.dart" ]; then
    entry="$app_dir/lib/main.dart"
  elif [ -f "$app_dir/packages/sample/lib/devtools.dart" ]; then
    entry="$app_dir/packages/sample/lib/devtools.dart"
  else
    die "no entrypoint found; pass --entry"
  fi
fi

dartaotruntime="$ENGINE_TOOLCHAIN/dart-sdk/bin/dartaotruntime"
frontend="$ENGINE_TOOLCHAIN/engine-artifacts/frontend_server_aot.dart.snapshot"
gen_snapshot="${SKYLINE_GEN_SNAPSHOT:-}"
if [ -z "$gen_snapshot" ]; then
  # Use the gen_snapshot built with the pinned release engine: the downloaded
  # toolchain binary is a JIT-release build and produces snapshots whose VM
  # configuration does not match a product engine.  Set SKYLINE_GEN_SNAPSHOT
  # to use another binary explicitly.
  gen_snapshot="$ENGINE_CHECKOUT/engine/src/out/host_release/gen_snapshot"
  [ -x "$gen_snapshot" ] \
    || die "release gen_snapshot missing at $gen_snapshot; run: npm run engine:build -- --runtime-mode release (or set SKYLINE_GEN_SNAPSHOT)"
fi
package_config="$app_dir/package_config.json"
assets="$app_dir/build/flutter_assets"
out_lib="$app_dir/build/libapp.so"

if [ "$patched_sdk" -eq 1 ]; then
  sdk_root="$ENGINE_PATCHED_SDK"
else
  sdk_root="$ENGINE_TOOLCHAIN/flutter_patched_sdk"
fi
for f in "$dartaotruntime" "$frontend" "$gen_snapshot" \
         "$sdk_root/platform_strong.dill" "$entry" "$package_config"; do
  [ -e "$f" ] || die "missing $f; run: npm run toolchain:fetch (and npm run ui:patch for --patched-sdk)"
done

mkdir -p "$assets"
tmp_kernel=$(mktemp --suffix=.dill)
trap 'rm -f "$tmp_kernel"' EXIT

log "compiling AOT kernel ($entry)"
"$dartaotruntime" "$frontend" \
  --sdk-root "$sdk_root/" \
  --target=flutter \
  --target-os="$target_os" \
  --aot \
  --tfa \
  --no-print-incremental-dependencies \
  --packages "$package_config" \
  --output-dill "$tmp_kernel" \
  "$entry"

[ -s "$tmp_kernel" ] || die "frontend_server produced no AOT kernel"

rm -f "$out_lib"
log "generating AOT snapshot"
gen_flags=(--deterministic --snapshot_kind=app-aot-elf --elf="$out_lib" --strip)
# Product-mode gen_snapshot always deduplicates instructions and rejects the
# flag; the JIT-release toolchain build defaults to false and needs it to match
# a product engine.
if "$gen_snapshot" --help --verbose 2>/dev/null | grep -q '^dedup_instructions:'; then
  gen_flags+=(--dedup_instructions)
fi
"$gen_snapshot" "${gen_flags[@]}" "$tmp_kernel"

[ -s "$out_lib" ] || die "gen_snapshot produced no libapp.so"
size=$(wc -c < "$out_lib")
sha=$(sha256sum "$out_lib" | awk '{print $1}')
log "libapp.so: $size bytes, sha256 $sha"

stamp_args=(stamp aot --app-dir "$app_dir")
[ "$patched_sdk" -eq 1 ] && stamp_args+=(--patched-sdk)
node "$ENGINE_REPO_ROOT/tools/build-status.js" "${stamp_args[@]}"

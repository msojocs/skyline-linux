#!/usr/bin/env bash
# Recompiles the official Skyline Flutter application (`package:skyline` +
# `package:sample/devtools.dart`) against the public Dart 3.9.2 SDK and the
# Skyline `dart:ui` compatibility layer.
#
#   bash native/engine/build-skyline-kernel.sh [--refresh-sources] [--no-assets] [--check]
#
# By default the non-VM assets (fonts, images, AssetManifest) are copied from the
# official bundle discovered by test/lib/env.js; --no-assets skips that step.
#
# Prerequisites:
#   npm run toolchain:fetch          # Dart SDK + frontend_server
#   npm run engine:fetch             # engine checkout (for the patched SDK)
#   bash native/engine/patch-dart-ui.sh   # builds and stages the patched SDK
#
# Sources are reconstructed from the official Windows kernel_blob.bin (the
# kernel embeds uriToSource for every compiled package).  They are generated
# under native/engine/work/ and are not committed: the repository ships no
# official binaries or proprietary sources.  The Linux compat changes are the
# unified diffs in native/engine/app_compat/*.patch; --refresh-sources
# regenerates pristine sources before applying them (needed when a patch no
# longer applies, e.g. after the sources were edited by hand).
#
# --check   verify the patches and the kernel build stamp without writing
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

refresh_sources=0
check_only=0
copy_assets=1
for arg in "$@"; do
  case "$arg" in
    --refresh-sources) refresh_sources=1 ;;
    --no-assets) copy_assets=0 ;;
    --check) check_only=1 ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
done

dartaotruntime="$ENGINE_TOOLCHAIN/dart-sdk/bin/dartaotruntime"
frontend="$ENGINE_TOOLCHAIN/engine-artifacts/frontend_server_aot.dart.snapshot"
app="$ENGINE_SKYLINE_APP"
root_library="$app/packages/sample/lib/devtools.dart"
package_config="$app/package_config.json"
assets="$app/build/flutter_assets"
out="$assets/kernel_blob.bin"

for f in "$dartaotruntime" "$frontend"; do
  [ -e "$f" ] || die "missing $f; run: npm run toolchain:fetch"
done

if [ "$refresh_sources" -eq 1 ] || [ ! -f "$root_library" ] || [ ! -f "$package_config" ]; then
  [ "$check_only" -eq 1 ] && die "reconstructed sources missing; run without --check"
  log "reconstructing package sources from the official kernel"
  node "$ENGINE_REPO_ROOT/tools/reconstruct-kernel-sources.js" --out "$app"
fi

if [ ! -f "$ENGINE_PATCHED_SDK/platform_strong.dill" ]; then
  die "patched SDK missing at $ENGINE_PATCHED_SDK; run: bash native/engine/patch-dart-ui.sh"
fi

app_compat_dir="$ENGINE_DIR/app_compat"

# git apply on the reconstructed tree.  It lives inside this (ignored)
# repository, so stop repository discovery at its parent: git then patches it
# as a plain directory, like GNU patch without fuzz.
app_git_apply() {
  (cd "$app" && GIT_CEILING_DIRECTORIES="$(dirname "$app")" git apply "$@")
}

app_compat_patches() {
  find "$app_compat_dir" -maxdepth 1 -name '*.patch' -print | sort
}

apply_app_compat() {
  local patch name
  while IFS= read -r patch; do
    name=$(basename "$patch")
    if app_git_apply --reverse --check "$patch" >/dev/null 2>&1; then
      log "app compat already applied: $name"
    elif app_git_apply --check "$patch" >/dev/null 2>&1; then
      app_git_apply "$patch"
      log "app compat applied: $name"
    else
      die "app compat patch $name does not apply to $app (sources edited or reconstructed from another kernel); run: npm run kernel:skyline -- --refresh-sources"
    fi
  done < <(app_compat_patches)
}

if [ "$check_only" -eq 1 ]; then
  while IFS= read -r patch; do
    app_git_apply --reverse --check "$patch" >/dev/null 2>&1 ||
      die "app compat patch not applied: $(basename "$patch") (run without --check)"
  done < <(app_compat_patches)
  [ -s "$out" ] || die "missing $out; run without --check"
  hash=$(revision_value PUBLIC_KERNEL_SDK_HASH)
  if [ -n "$hash" ] && ! head -c 64 "$out" | grep -aq "$hash"; then
    die "kernel header does not contain the public SDK hash $hash"
  fi
  node "$ENGINE_REPO_ROOT/tools/build-status.js" --check kernel >&2 ||
    die "kernel is stale (see above); run without --check"
  log "kernel ok: $out"
  exit 0
fi

apply_app_compat

mkdir -p "$assets"
rm -f "$out"

if [ "$copy_assets" -eq 1 ]; then
  bundle=$(node -e '
    try {
      const { oraclePaths } = require(process.argv[1]);
      const paths = oraclePaths();
      process.stdout.write(paths ? paths.bundle : "");
    } catch {}
  ' "$ENGINE_REPO_ROOT/test/lib/env.js")
  if [ -n "$bundle" ] && [ -d "$bundle/flutter_assets" ]; then
    log "copying official non-VM assets from $bundle/flutter_assets"
    tar -C "$bundle/flutter_assets" -cf - \
      --exclude=./kernel_blob.bin \
      --exclude=./isolate_snapshot_data \
      --exclude=./vm_snapshot_data . | tar -C "$assets" -xf -
  else
    warn "official bundle not found; the appservice UI may miss fonts/images"
  fi
fi

log "compiling $root_library"
"$dartaotruntime" "$frontend" \
  --sdk-root "$ENGINE_PATCHED_SDK/" \
  --target=flutter \
  --no-print-incremental-dependencies \
  --packages "$package_config" \
  --output-dill "$out" \
  "$root_library"

[ -s "$out" ] || die "frontend_server produced no kernel blob"

expected_hash=$(revision_value PUBLIC_KERNEL_SDK_HASH)
if [ -n "$expected_hash" ] && ! head -c 64 "$out" | grep -aq "$expected_hash"; then
  warn "kernel header does not contain the staged SDK hash $expected_hash"
fi

size=$(wc -c < "$out")
sha=$(sha256sum "$out" | awk '{print $1}')
log "kernel_blob.bin: $size bytes, sha256 $sha"
node "$ENGINE_REPO_ROOT/tools/build-status.js" stamp kernel
log "assets ready: $assets"

#!/usr/bin/env bash
# Packs the prebuilt release engine and AOT bundle that CI downloads from the
# `build-deps` release (.github/workflows/release.yml).
#
#   bash native/engine/pack-build-deps.sh [--output DIR]
#
# The engine embeds the Skyline runtime and the bundle holds the AOT snapshot of
# the recompiled package:skyline, so CI cannot rebuild either; it only checks
# that they match the commit it builds (tools/build-status.js --check
# engine-release,aot).  This script refuses stale artifacts and writes
#
#   DIR/skyline-linux-deps-linux-x64.tar.gz   engine/ + bundle/ + BUILD-DEPS.json
#   DIR/skyline-linux-deps-linux-x64.tar.gz.sha256
#
# Prerequisites (release):
#   npm run runtime:embed -- --runtime-mode release
#   npm run devtools:aot
#
# Publish (replaces the assets CI downloads):
#   gh release upload build-deps DIR/skyline-linux-deps-linux-x64.tar.gz \
#     DIR/skyline-linux-deps-linux-x64.tar.gz.sha256 --clobber
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

output="$ENGINE_REPO_ROOT/dist"
while [ $# -gt 0 ]; do
  case "$1" in
    --output)
      [ $# -ge 2 ] || die "--output requires a value"
      output=$2
      shift 2
      ;;
    -h|--help)
      sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
done

stage="$ENGINE_DIR/out/linux-x64-release"
build="$ENGINE_SKYLINE_APP/build"
for f in "$stage/libflutter_engine.so" "$stage/flutter_embedder.h" "$stage/icudtl.dat" \
         "$stage/skyline-build.json" "$build/libapp.so" "$build/skyline-build.aot.json" \
         "$build/flutter_assets/FontManifest.json"; do
  [ -f "$f" ] || die "missing $f; build the release engine and the AOT bundle first"
done
node "$ENGINE_REPO_ROOT/tools/build-status.js" --check engine-release,aot >&2 ||
  die "the release engine or AOT bundle is stale (see above); rebuild before packing"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/engine" "$tmp/bundle"
cp "$stage/libflutter_engine.so" "$stage/flutter_embedder.h" "$stage/icudtl.dat" \
  "$stage/skyline-build.json" "$tmp/engine/"
cp -a "$build/flutter_assets" "$tmp/bundle/flutter_assets"
# The release engine runs the AOT snapshot; the JIT kernel is not shipped.
rm -f "$tmp/bundle/flutter_assets/kernel_blob.bin"
cp "$build/libapp.so" "$build/skyline-build.aot.json" "$tmp/bundle/"
node "$ENGINE_REPO_ROOT/tools/build-status.js" --json --check engine-release,aot \
  > "$tmp/BUILD-DEPS.json"

mkdir -p "$output"
archive="$output/skyline-linux-deps-linux-x64.tar.gz"
tar -czf "$archive" -C "$tmp" engine bundle BUILD-DEPS.json
(cd "$output" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
log "wrote $archive"
cat "$archive.sha256" >&2

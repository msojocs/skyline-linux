#!/usr/bin/env bash
# Downloads the Dart/Flutter toolchain used to compile the minimal dart:ui app and
# to run the headless host (icudtl.dat).  Does NOT download the engine checkout.
#
#   bash native/engine/fetch-toolchain.sh [--check] [--force] [--with-gtk]
#
# Artifacts and SHA-256 values live in ARTIFACTS.json.  Set SKYLINE_PROXY to route
# curl through a proxy, e.g. SKYLINE_PROXY=http://127.0.0.1:54321.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

check_only=0
force=0
with_gtk=0
for arg in "$@"; do
  case "$arg" in
    --check) check_only=1 ;;
    --force) force=1 ;;
    --with-gtk) with_gtk=1 ;;
    -h|--help)
      sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $arg" ;;
  esac
done

require_cmd node
require_cmd sha256sum
if [ "$check_only" -eq 0 ]; then
  require_cmd curl
  require_cmd unzip
fi

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

manifest_tsv() {
  node -e '
    const fs = require("fs");
    const m = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
    for (const a of m.artifacts) {
      const url = a.url.replace("{{base}}", m.baseUrl);
      process.stdout.write([
        a.name, url, a.sha256, String(a.size), a.file, a.extractTo,
        a.required ? "1" : "0", (a.provides || []).join(","),
      ].join("\t") + "\n");
    }
  ' "$ENGINE_DIR/ARTIFACTS.json"
}

missing=0

headers_tsv() {
  node -e '
    const fs = require("fs");
    const m = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
    for (const h of (m.headers || [])) {
      process.stdout.write([
        h.name, h.url, h.sha256, String(h.size), h.file, h.to,
        h.required ? "1" : "0",
      ].join("\t") + "\n");
    }
  ' "$ENGINE_DIR/ARTIFACTS.json"
}

while IFS=$'\t' read -r name url sha size file to required; do
  [ -n "$name" ] || continue
  [ "$required" = "1" ] || [ "$with_gtk" -eq 1 ] || continue
  dest="$ENGINE_TOOLCHAIN/$to"
  if [ -f "$dest" ] && [ "$(sha256_file "$dest")" = "$sha" ]; then
    log "$name: header ok"
    continue
  fi
  if [ "$check_only" -eq 1 ]; then
    missing=1
    log "$name: missing or hash mismatch ($dest)"
    continue
  fi
  mkdir -p "$(dirname "$dest")"
  tmp="$dest.part"
  rm -f "$tmp"
  log "$name: downloading $url"
  curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
  actual=$(sha256_file "$tmp")
  if [ "$actual" != "$sha" ]; then
    rm -f "$tmp"
    die "$name: sha256 mismatch (expected $sha, got $actual)"
  fi
  mv "$tmp" "$dest"
done < <(headers_tsv)

while IFS=$'\t' read -r name url sha size file extract_to required provides; do
  [ "$required" = "1" ] || [ "$with_gtk" -eq 1 ] || continue
  zip_path="$ENGINE_DOWNLOADS/$file"
  ok=1
  if [ ! -f "$zip_path" ]; then
    ok=0
  elif [ "$(sha256_file "$zip_path")" != "$sha" ]; then
    ok=0
  fi
  if [ "$ok" -eq 1 ]; then
    log "$name: archive ok"
  else
    if [ "$check_only" -eq 1 ]; then
      missing=1
      log "$name: missing or hash mismatch ($zip_path)"
      continue
    fi
    mkdir -p "$ENGINE_DOWNLOADS"
    log "$name: downloading $url"
    tmp="$zip_path.part"
    rm -f "$tmp"
    curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
    actual=$(sha256_file "$tmp")
    if [ "$actual" != "$sha" ]; then
      rm -f "$tmp"
      die "$name: sha256 mismatch (expected $sha, got $actual)"
    fi
    mv "$tmp" "$zip_path"
    log "$name: downloaded ($size bytes)"
  fi
  if [ "$check_only" -eq 0 ]; then
    dest="$ENGINE_TOOLCHAIN/$extract_to"
    mkdir -p "$dest"
    unzip -q -o "$zip_path" -d "$dest"
  fi
  IFS=',' read -ra entries <<< "$provides"
  for entry in "${entries[@]}"; do
    [ -z "$entry" ] && continue
    if [ ! -e "$ENGINE_TOOLCHAIN/$entry" ]; then
      if [ "$check_only" -eq 1 ]; then
        missing=1
        log "$name: missing extracted file $entry"
      else
        die "$name: expected $entry after extraction"
      fi
    fi
  done
done < <(manifest_tsv)

if [ "$check_only" -eq 0 ]; then
  chmod +x "$ENGINE_TOOLCHAIN/dart-sdk/bin/dartaotruntime" 2>/dev/null || true
  chmod +x "$ENGINE_TOOLCHAIN/dart-sdk/bin/dart" 2>/dev/null || true
  chmod +x "$ENGINE_TOOLCHAIN/engine-artifacts/gen_snapshot" 2>/dev/null || true
  log "toolchain ready in $ENGINE_TOOLCHAIN"
fi

if [ "$missing" -ne 0 ] && [ "$check_only" -eq 1 ]; then
  die "toolchain is incomplete; run without --check to fetch it"
fi

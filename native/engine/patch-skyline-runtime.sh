#!/usr/bin/env bash
# Installs the embedded Skyline runtime into the pinned engine checkout and
# builds `libflutter_engine.so` with the 73 addon exports and the synthetic
# Skyline page path.
#
#   bash native/engine/patch-skyline-runtime.sh [--check] [--no-build]
#                                                 [--runtime-mode debug|release]
#
# The embedded build has the full Dart API (statically linked), so it can call
# the real `package:skyline` widget constructors (NewYGNode/NewDecoratedBox/
# CreatePageRoute).  `native/runtime/skyline_runtime_stub.cc` is reused with
# SKYLINE_EMBEDDED defined; the standalone DL-only library remains available as
# `native/build/libskyline.so` for bootstrap-only tests.
#
# After building, `libskyline.so` is symlinked next to the engine so the
# package:skyline Linux lookup (`DynamicLibrary.open("libskyline.so")`) resolves.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

engine_root="$ENGINE_CHECKOUT/engine/src"
embedder_dir="$engine_root/flutter/shell/platform/embedder"
source_file="$ENGINE_REPO_ROOT/native/runtime/skyline_runtime_stub.cc"
build_file="$embedder_dir/BUILD.gn"
export_file="$embedder_dir/embedder_exports.lst"
link_path="$ENGINE_STAGE/libskyline.so"

check_only=0
do_build=1
runtime_mode=debug
while [ $# -gt 0 ]; do
  case "$1" in
    --check) check_only=1 ;;
    --no-build) do_build=0 ;;
    --runtime-mode)
      [ $# -ge 2 ] || die "--runtime-mode needs a value"
      runtime_mode=$2
      shift
      ;;
    --runtime-mode=*) runtime_mode=${1#*=} ;;
    -h|--help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done
case "$runtime_mode" in
  debug) ENGINE_STAGE="$ENGINE_DIR/out/linux-x64-debug" ;;
  release) ENGINE_STAGE="$ENGINE_DIR/out/linux-x64-release" ;;
  *) die "unsupported runtime mode: $runtime_mode" ;;
esac
export SKYLINE_ENGINE_STAGE="$ENGINE_STAGE"
link_path="$ENGINE_STAGE/libskyline.so"

[ -d "$embedder_dir" ] || die "missing engine checkout: $embedder_dir (run npm run engine:fetch)"
[ -f "$source_file" ] || die "missing runtime source: $source_file"

if [ "$check_only" -eq 1 ]; then
  [ -f "$embedder_dir/skyline_runtime.cc" ] || die "embedded runtime not installed"
  grep -q '"skyline_runtime.cc"' "$build_file" || die "BUILD.gn does not compile skyline_runtime.cc"
  grep -q 'AdjustDecelerationVelocity;' "$export_file" || die "embedder_exports.lst lacks the Skyline exports"
  [ -e "$link_path" ] || die "missing $link_path (run without --check)"
  log "embedded Skyline runtime is installed"
  exit 0
fi

{
  echo '#define SKYLINE_EMBEDDED 1'
  cat "$source_file"
} > "$embedder_dir/skyline_runtime.cc"

python3 - "$embedder_dir" "$ENGINE_REPO_ROOT/spec/kernel-symbols.json" <<'PY'
import json
import sys
from pathlib import Path

embedder_dir = Path(sys.argv[1])
spec_path = Path(sys.argv[2])

build_path = embedder_dir / "BUILD.gn"
build = build_path.read_text(encoding="utf-8")
if '"skyline_runtime.cc"' not in build:
    anchor = '''      "vsync_waiter_embedder.cc",
      "vsync_waiter_embedder.h",
    ]

    public_deps = [ ":embedder_headers" ]'''
    replacement = '''      "vsync_waiter_embedder.cc",
      "vsync_waiter_embedder.h",
    ]

    # Phase 3: Skyline runtime with full Dart API access (public embedder only).
    if (target_name == "embedder") {
      sources += [ "skyline_runtime.cc" ]
    }

    public_deps = [ ":embedder_headers" ]'''
    if anchor not in build:
        raise SystemExit("cannot find embedder BUILD.gn sources anchor")
    build = build.replace(anchor, replacement, 1)
    build_path.write_text(build, encoding="utf-8")
    print("patched BUILD.gn")

export_path = embedder_dir / "embedder_exports.lst"
exports = export_path.read_text(encoding="utf-8")
spec = json.loads(spec_path.read_text(encoding="utf-8"))
names = sorted(entry["name"] for entry in spec["ffi"]["lookupSymbols"])
# Host-injected keyboard channel (not part of the captured FFI table).
names.append("DispatchSkylineKeyEvent")
missing = [name for name in names if f"    {name};" not in exports]
if missing:
    anchor = "    kInternalFlutterGpu*;\n"
    if anchor not in exports:
        raise SystemExit("cannot find embedder_exports.lst anchor")
    block = "    # Skyline runtime exports (phase 3)\n" + "".join(
        f"    {name};\n" for name in missing
    )
    exports = exports.replace(anchor, anchor + block, 1)
    export_path.write_text(exports, encoding="utf-8")
    print(f"patched embedder_exports.lst ({len(missing)} symbols)")
PY

if [ "$do_build" -eq 1 ]; then
  log "building flutter_engine with the embedded Skyline runtime ($runtime_mode)"
  bash "$SCRIPT_DIR/build-engine.sh" --runtime-mode "$runtime_mode"
fi

[ -f "$ENGINE_STAGE/libflutter_engine.so" ] || die "engine not staged; run npm run engine:build"
ln -sfn "$ENGINE_STAGE/libflutter_engine.so" "$link_path"

expected=$(node -e '
  const fs = require("fs");
  const spec = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  process.stdout.write(String(spec.ffi.lookupSymbols.length));
' "$ENGINE_REPO_ROOT/spec/kernel-symbols.json")
exported=$(nm -D --defined-only "$ENGINE_STAGE/libflutter_engine.so" |
  awk '{print $NF}' |
  node -e '
    const fs = require("fs");
    const want = new Set(JSON.parse(fs.readFileSync(process.argv[1], "utf8")).ffi.lookupSymbols.map(e => e.name));
    let count = 0;
    for (const line of fs.readFileSync(0, "utf8").split("\n")) {
      if (want.has(line.trim())) count++;
    }
    process.stdout.write(String(count));
  ' "$ENGINE_REPO_ROOT/spec/kernel-symbols.json")
if [ "$exported" != "$expected" ]; then
  die "engine exports $exported/$expected Skyline runtime symbols"
fi
log "embedded Skyline runtime ready: $link_path -> libflutter_engine.so ($exported symbols)"

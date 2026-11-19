#!/usr/bin/env python3
"""Applies the minimal Linux-compat fix to the reconstructed package:skyline.

`YGConnector.updateChildren`/`removeChild` call `coordinateStackingChildren()`
unconditionally.  That method asserts `enableStackingPaint`; connectors created
without stacking (our Linux runtime) therefore throw when the native side
updates children.  Guard the call; non-stacking connectors paint via firstChild.

Usage: apply.py <reconstructed app dir>
"""

import sys
from pathlib import Path

app = Path(sys.argv[1])
path = app / "packages" / "skyline" / "lib" / "widgets" / "yoga.dart"
text = path.read_text(encoding="utf-8")

guarded = (
    "    if (renderObject?.enableStackingPaint ?? false) {\n"
    "      renderObject?.coordinateStackingChildren();\n"
    "    }"
)

changed = False
# removeChild
old = (
    "    // Proactively update stacking children in case it's paint method is not\n"
    "    // being called somehow, which leads to memory leak and hitTest issue.\n"
    "    renderObject?.coordinateStackingChildren();\n"
    "    assert(!children.contains(child));"
)
new = (
    "    // Proactively update stacking children in case it's paint method is not\n"
    "    // being called somehow, which leads to memory leak and hitTest issue.\n"
    + guarded + "\n"
    "    assert(!children.contains(child));"
)
if old in text:
    text = text.replace(old, new, 1)
    changed = True

# updateChildren
old_update = (
    "    // Proactively update stacking children in case it's paint method is not\n"
    "    // being called somehow, which leads to memory leak and hitTest issue.\n"
    "    renderObject?.coordinateStackingChildren();\n"
    "    markNeedsLayout();"
)
new_update = (
    "    // Proactively update stacking children in case it's paint method is not\n"
    "    // being called somehow, which leads to memory leak and hitTest issue.\n"
    "    // Linux compat: non-stacking connectors must not call this (it asserts\n"
    "    // enableStackingPaint); they paint via firstChild anyway.\n"
    + guarded + "\n"
    "    markNeedsLayout();"
)
if old_update in text:
    text = text.replace(old_update, new_update, 1)
    changed = True

if changed:
    path.write_text(text, encoding="utf-8")
    print(f"patched {path}")
else:
    if text.count(guarded) < 2:
        raise SystemExit(f"cannot find coordinateStackingChildren anchors in {path}")
    print(f"already patched {path}")

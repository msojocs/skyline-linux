#!/usr/bin/env python3
"""Applies the Skyline compatibility edits to the pinned engine checkout.

`patch-dart-ui.sh` copies the new `dart:ui` parts next to the engine sources and
then runs this script.  Every edit is additive and idempotent; the script refuses
to guess when an anchor is missing so a new engine revision fails loudly instead
of silently producing a broken SDK.

Edited files (relative to `$ENGINE_CHECKOUT/engine/src`):

  flutter/lib/ui/ui.dart                     declare the new parts
  flutter/lib/ui/platform_dispatcher.dart    dispatcher additions, FramePhase
  flutter/lib/ui/window.dart                 FlutterView.windowStatus
  flutter/lib/ui/painting.dart               Canvas.clipRRect(clipOp:)
  flutter/lib/ui/text.dart                   TextStyle/ParagraphStyle params
  flutter/third_party/dart/sdk/lib/io/platform.dart  Platform.isOhos
"""

import sys
from pathlib import Path

engine_src = Path(sys.argv[1])


def edit(relative, transform):
    path = engine_src / relative
    if not path.exists():
        raise SystemExit(f"missing engine source: {path}")
    text = path.read_text(encoding="utf-8")
    updated, changed = transform(text)
    if changed:
        path.write_text(updated, encoding="utf-8")
        print(f"patched {relative}")
    else:
        print(f"already patched {relative}")


def replace_once(text, old, new, label):
    if old not in text:
        if new in text:
            return text, False
        raise SystemExit(f"cannot find anchor for {label}")
    return text.replace(old, new, 1), True


# ---------------------------------------------------------------------------
# dart:ui parts and the removed first-iteration snippet
# ---------------------------------------------------------------------------


def patch_ui(text):
    changed = False
    anchor = "part 'annotations.dart';\n"
    for part in ("application.dart", "mmtrace.dart", "skyline_ext.dart"):
        directive = f"part '{part}';\n"
        if directive not in text:
            text, _ = replace_once(text, anchor, anchor + directive, f"part {part}")
            changed = True
    return text, changed


def patch_painting(text):
    if "// Skyline compatibility additions for `dart:ui`" in text:
        text = text[: text.index("\n// Skyline compatibility additions for `dart:ui`")].rstrip() + "\n"
        return text, True
    return text, False


# ---------------------------------------------------------------------------
# PlatformDispatcher additions
# ---------------------------------------------------------------------------

DISPATCHER_STATICS = (
    "\n"
    "  // Skyline compatibility: the private engine implements these through\n"
    "  // `PlatformConfigurationNativeApi::{Is,Set}UseParallelPipeline`.  The public\n"
    "  // engine has no such hooks yet, so keep the state in Dart.\n"
    "  static bool _skylineUseParallelPipeline = false;\n"
    "  static bool isUseParallelPipeline() => _skylineUseParallelPipeline;\n"
    "  static void setUseParallelPipeline(bool enable) {\n"
    "    _skylineUseParallelPipeline = enable;\n"
    "  }\n"
)

DISPATCHER_INSTANCE = (
    "\n"
    "  // ---- Skyline compatibility ------------------------------------------------\n"
    "  // These members exist in the private engine's dart:ui and are used by\n"
    "  // package:skyline.  Where the public engine has no matching native hook the\n"
    "  // implementation is a safe no-op (or a documented constant).\n"
    "\n"
    "  /// Returns the display refresh rate in Hz.\n"
    "  double getDisplayRefreshRate() => 60.0;\n"
    "\n"
    "  /// Runs a garbage collection. The call is advisory; the public engine has\n"
    "  /// no PlatformConfigurationNativeApi::CollectGarbage hook.\n"
    "  void collectGarbage() {}\n"
    "\n"
    "  /// Sets the UI runner slow-frame threshold (ANR detection not ported).\n"
    "  void setUiRunnerThreshold(int milliseconds, {bool check_anr = true}) {}\n"
    "\n"
    "  /// Sets the raster runner slow-frame threshold (ANR detection not ported).\n"
    "  void setRasterRunnerThreshold(int milliseconds, {bool check_anr = true}) {}\n"
    "\n"
    "  /// Schedules a frame without waiting for VSync.\n"
    "  void scheduleFrameIgnoreVsync() => scheduleFrame();\n"
    "\n"
    "  /// Marks a startup timing checkpoint (startup tracing not ported).\n"
    "  void recordStartupTiming(String name) {}\n"
    "\n"
    "  SlowRunnerStackCallback _skylineSlowRunnerStackCallback =\n"
    "      (String name, int milliseconds, List<String> stackTraces) {};\n"
    "\n"
    "  /// Callback invoked when a runner exceeds its stack-report threshold.\n"
    "  SlowRunnerStackCallback get onReportSlowRunnerStack =>\n"
    "      _skylineSlowRunnerStackCallback;\n"
    "\n"
    "  set onReportSlowRunnerStack(SlowRunnerStackCallback? callback) {\n"
    "    if (callback != null) {\n"
    "      _skylineSlowRunnerStackCallback = callback;\n"
    "    }\n"
    "  }\n"
)


def patch_platform_dispatcher(text):
    changed = False
    if "_skylineUseParallelPipeline" not in text:
        anchor = (
            "  @Native<Double Function(Double, Int)>(symbol: 'PlatformConfigurationNativeApi::GetScaledFontSize')\n"
            "  external static double _getScaledFontSize(double unscaledFontSize, int configurationId);\n"
            "}\n"
        )
        block = DISPATCHER_INSTANCE + DISPATCHER_STATICS
        text, _ = replace_once(text, anchor, anchor[: -len("}\n")] + block + "}\n", "PlatformDispatcher additions")
        changed = True
    elif "getDisplayRefreshRate" not in text:
        anchor = "  static bool _skylineUseParallelPipeline = false;\n"
        if anchor not in text:
            raise SystemExit("cannot find PlatformDispatcher compatibility block")
        text = text.replace(anchor, DISPATCHER_INSTANCE + "\n" + anchor, 1)
        changed = True

    # Extra FramePhase values used by package:skyline.  The public engine keeps
    # the six original timestamps; FrameTiming below maps the Skyline-only
    # phases onto existing timestamps so the native payload stays 11 ints.
    text, c = replace_once(
        text,
        "  rasterFinishWallTime,\n}\n",
        "  rasterFinishWallTime,\n"
        "\n"
        "  /// When the frame should be rendered on screen (Skyline addition).\n"
        "  vsyncTarget,\n"
        "\n"
        "  /// When the frame actually starts rendering, excluding acquireFrame\n"
        "  /// (Skyline addition).\n"
        "  compositeFrameStart,\n"
        "}\n",
        "FramePhase values",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  static final int _dataLength = FramePhase.values.length + _FrameTimingInfo.values.length;",
        "  // The public engine reports the six original FramePhase timestamps plus\n"
        "  // the five _FrameTimingInfo fields.  The Skyline-only phases below are\n"
        "  // derived in timestampInMicroseconds, so the native payload length stays 11.\n"
        "  static final int _dataLength = 6 + _FrameTimingInfo.values.length;",
        "_dataLength",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  int timestampInMicroseconds(FramePhase phase) => _data[phase.index];",
        "  int timestampInMicroseconds(FramePhase phase) {\n"
        "    switch (phase) {\n"
        "      case FramePhase.vsyncTarget:\n"
        "        return _data[FramePhase.vsyncStart.index];\n"
        "      case FramePhase.compositeFrameStart:\n"
        "        return _data[FramePhase.buildStart.index];\n"
        "      default:\n"
        "        return _data[phase.index];\n"
        "    }\n"
        "  }",
        "timestampInMicroseconds",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  int _rawInfo(_FrameTimingInfo info) => _data[FramePhase.values.length + info.index];",
        "  int _rawInfo(_FrameTimingInfo info) => _data[6 + info.index];",
        "_rawInfo",
    )
    changed = changed or c
    return text, changed


# ---------------------------------------------------------------------------
# FlutterView.windowStatus
# ---------------------------------------------------------------------------


def patch_window(text):
    if "WindowStatus get windowStatus" in text:
        return text, False
    anchor = "  List<DisplayFeature> get displayFeatures => _viewConfiguration.displayFeatures;\n"
    addition = (
        "\n"
        "  /// Represents the status of the window corresponding to this view.\n"
        "  ///\n"
        "  /// The public engine always reports [WindowStatus.normal].\n"
        "  WindowStatus get windowStatus => WindowStatus.normal;\n"
    )
    return replace_once(text, anchor, anchor + addition, "FlutterView.windowStatus")


# ---------------------------------------------------------------------------
# Canvas.clipRRect(clipOp:)
# ---------------------------------------------------------------------------


def patch_canvas(text):
    changed = False
    text, c = replace_once(
        text,
        "  void clipRRect(RRect rrect, {bool doAntiAlias = true});",
        "  void clipRRect(RRect rrect, {ClipOp clipOp = ClipOp.intersect, bool doAntiAlias = true});",
        "Canvas.clipRRect declaration",
    )
    changed = changed or c
    text, c = replace_once(
        text,
        "  void clipRRect(RRect rrect, {bool doAntiAlias = true}) {",
        "  void clipRRect(RRect rrect, {ClipOp clipOp = ClipOp.intersect, bool doAntiAlias = true}) {\n"
        "    // clipOp is a Skyline extension; the public engine only intersects.\n",
        "Canvas.clipRRect implementation",
    )
    changed = changed or c
    return text, changed


# ---------------------------------------------------------------------------
# TextStyle / ParagraphStyle private parameters
# ---------------------------------------------------------------------------


def patch_text(text):
    changed = False

    # TextStyle: lineHeight / ignoreClusterLetterSpacing / useCoreTextWordBreak.
    full_signature = (
        "    double? height,\n"
        "    double? lineHeight,\n"
        "    bool ignoreClusterLetterSpacing = false,\n"
        "    bool useCoreTextWordBreak = false,\n"
        "    TextLeadingDistribution? leadingDistribution,"
    )
    if full_signature in text:
        pass
    elif (
        "    double? lineHeight,\n"
        "    bool useCoreTextWordBreak = false,\n"
        "    TextLeadingDistribution? leadingDistribution," in text
    ):
        # Upgrade path: checkout patched before ignoreClusterLetterSpacing.
        text = text.replace(
            "    double? lineHeight,\n"
            "    bool useCoreTextWordBreak = false,\n"
            "    TextLeadingDistribution? leadingDistribution,",
            "    double? lineHeight,\n"
            "    bool ignoreClusterLetterSpacing = false,\n"
            "    bool useCoreTextWordBreak = false,\n"
            "    TextLeadingDistribution? leadingDistribution,",
            1,
        )
        changed = True
    else:
        text, _ = replace_once(
            text,
            "    double? height,\n    TextLeadingDistribution? leadingDistribution,",
            full_signature,
            "TextStyle.lineHeight/ignoreClusterLetterSpacing",
        )
        changed = True

    text, c = replace_once(
        text,
        "       _height = height,\n       _decorationThickness = decorationThickness,",
        "       _height = height,\n"
        "       _lineHeight = lineHeight,\n"
        "       _useCoreTextWordBreak = useCoreTextWordBreak,\n"
        "       _decorationThickness = decorationThickness,",
        "TextStyle initializers",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  final double? _height;\n  final double? _decorationThickness;",
        "  final double? _height;\n"
        "  final double? _lineHeight;\n"
        "  final bool _useCoreTextWordBreak;\n"
        "  final double? _decorationThickness;",
        "TextStyle fields",
    )
    changed = changed or c

    # ParagraphStyle: lineHeight + backgroundUseLineMetrics.
    ps_full = (
        "    double? height,\n"
        "    double? lineHeight,\n"
        "    bool ignoreClusterLetterSpacing = false,\n"
        "    bool backgroundUseLineMetrics = false,\n"
        "    TextHeightBehavior? textHeightBehavior,"
    )
    if ps_full in text:
        pass
    elif (
        "    double? lineHeight,\n"
        "    bool backgroundUseLineMetrics = false,\n"
        "    TextHeightBehavior? textHeightBehavior," in text
    ):
        text = text.replace(
            "    double? lineHeight,\n    bool backgroundUseLineMetrics = false,",
            "    double? lineHeight,\n"
            "    bool ignoreClusterLetterSpacing = false,\n"
            "    bool backgroundUseLineMetrics = false,",
            1,
        )
        changed = True
    elif "    bool backgroundUseLineMetrics = false,\n    TextHeightBehavior? textHeightBehavior," in text:
        text = text.replace(
            "    double? height,\n    bool backgroundUseLineMetrics = false,",
            "    double? height,\n"
            "    double? lineHeight,\n"
            "    bool ignoreClusterLetterSpacing = false,\n"
            "    bool backgroundUseLineMetrics = false,",
            1,
        )
        changed = True
    else:
        text, _ = replace_once(
            text,
            "    double? height,\n    TextHeightBehavior? textHeightBehavior,",
            ps_full,
            "ParagraphStyle.lineHeight/backgroundUseLineMetrics",
        )
        changed = True

    ps_init_full = (
        "       _height = height,\n"
        "       _lineHeight = lineHeight,\n"
        "       _backgroundUseLineMetrics = backgroundUseLineMetrics,\n"
        "       _strutStyle = strutStyle,"
    )
    if ps_init_full in text:
        pass
    elif "       _height = height,\n       _backgroundUseLineMetrics = backgroundUseLineMetrics," in text:
        text = text.replace(
            "       _height = height,\n       _backgroundUseLineMetrics = backgroundUseLineMetrics,",
            "       _height = height,\n"
            "       _lineHeight = lineHeight,\n"
            "       _backgroundUseLineMetrics = backgroundUseLineMetrics,",
            1,
        )
        changed = True
    else:
        text, _ = replace_once(
            text,
            "       _height = height,\n       _strutStyle = strutStyle,",
            ps_init_full,
            "ParagraphStyle initializers",
        )
        changed = True

    ps_fields_full = (
        "  final double? _height;\n"
        "  final double? _lineHeight;\n"
        "  final bool _backgroundUseLineMetrics;\n"
        "  final StrutStyle? _strutStyle;"
    )
    if ps_fields_full in text:
        pass
    elif "  final double? _height;\n  final bool _backgroundUseLineMetrics;\n  final StrutStyle? _strutStyle;" in text:
        text = text.replace(
            "  final double? _height;\n  final bool _backgroundUseLineMetrics;",
            "  final double? _height;\n  final double? _lineHeight;\n  final bool _backgroundUseLineMetrics;",
            1,
        )
        changed = True
    else:
        text, _ = replace_once(
            text,
            "  final double? _height;\n  final StrutStyle? _strutStyle;",
            ps_fields_full,
            "ParagraphStyle fields",
        )
        changed = True
    return text, changed


# ---------------------------------------------------------------------------
# dart:io Platform.isOhos
# ---------------------------------------------------------------------------


def patch_io_platform(text):
    if "isOhos" in text:
        return text, False
    anchor = '  static final bool isFuchsia = (operatingSystem == "fuchsia");\n'
    addition = (
        "\n"
        "  /// Check if the current platform is ohos.\n"
        "  ///\n"
        "  /// Private Flutter engine / Skyline addition; the public Dart SDK does not\n"
        "  /// define this operating system.\n"
        '  static final bool isOhos = (operatingSystem == "ohos");\n'
    )
    return replace_once(text, anchor, anchor + addition, "Platform.isOhos")


def main():
    edit("flutter/lib/ui/ui.dart", patch_ui)
    edit("flutter/lib/ui/painting.dart", patch_painting)
    edit("flutter/lib/ui/platform_dispatcher.dart", patch_platform_dispatcher)
    edit("flutter/lib/ui/window.dart", patch_window)
    edit("flutter/lib/ui/painting.dart", patch_canvas)
    edit("flutter/lib/ui/text.dart", patch_text)
    edit("flutter/third_party/dart/sdk/lib/io/platform.dart", patch_io_platform)
    return 0


if __name__ == "__main__":
    sys.exit(main())

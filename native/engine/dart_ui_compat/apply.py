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
  flutter/lib/ui/text/paragraph_builder.h    lineHeight native signatures
  flutter/lib/ui/text/paragraph_builder.cc   decode private text flags
  flutter/txt/src/txt/{text,paragraph}_style  carry the flags into txt
  flutter/txt/src/skia/paragraph_builder_skia.cc  map flags into SkParagraph
  flutter/third_party/skia/modules/skparagraph
                                             backgroundUseLineMetrics
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
    # Check the patched form first: several edits append to an anchor, so after
    # the first run `old` is still present as a prefix of `new`.
    if new in text:
        return text, False
    if old not in text:
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
    if "void clipRRect(RRect rrect, {ClipOp clipOp = ClipOp.intersect, bool doAntiAlias = true}) {" not in text:
        text, c = replace_once(
            text,
            "  void clipRRect(RRect rrect, {bool doAntiAlias = true}) {",
            "  void clipRRect(RRect rrect, {ClipOp clipOp = ClipOp.intersect, bool doAntiAlias = true}) {\n"
            "    // clipOp is a Skyline extension; the public engine only intersects.\n",
            "Canvas.clipRRect implementation",
        )
        changed = changed or c
    # Skyline uses ClipOp.difference.  The public native method only supports
    # intersection, so emulate difference by clipping to the complement of the
    # rounded rect (a large outer rect minus the rrect).
    if "clipPath(Path.combine(PathOperation.difference, outer, inner)," not in text:
        text, c = replace_once(
            text,
            "    // clipOp is a Skyline extension; the public engine only intersects.\n"
            "\n"
            "    assert(_rrectIsValid(rrect));\n"
            "    _clipRRect(rrect._getValue32(), doAntiAlias);\n",
            "    assert(_rrectIsValid(rrect));\n"
            "    if (clipOp == ClipOp.difference) {\n"
            "      final Path outer = Path()\n"
            "        ..addRect(rrect.outerRect.inflate(100000.0));\n"
            "      final Path inner = Path()..addRRect(rrect);\n"
            "      clipPath(Path.combine(PathOperation.difference, outer, inner),\n"
            "          doAntiAlias: doAntiAlias);\n"
            "      return;\n"
            "    }\n"
            "    _clipRRect(rrect._getValue32(), doAntiAlias);\n",
            "Canvas.clipRRect difference",
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

    text, c = patch_text_encoding(text)
    changed = changed or c
    return text, changed


# ---------------------------------------------------------------------------
# Private dart:ui text flag encoding
# ---------------------------------------------------------------------------
#
# The private engine's `dart:ui` encodes three additions that are not in the
# public text format (see the recovered source in
# `docs/phase-3-dart-ui-compat.md`):
#
#   TextStyle      bit 20: lineHeight (passed separately)
#                  bit 21: ignoreClusterLetterSpacing
#                  bit 22: useCoreTextWordBreak
#   ParagraphStyle bit 13: lineHeight (passed separately)
#                  bit 14: ignoreClusterLetterSpacing
#                  bit 15: backgroundUseLineMetrics
#
# `ParagraphBuilder::Create`/`pushStyle` also receive `lineHeight` as a
# dedicated double, and the native side decodes the flags in
# `flutter/lib/ui/text/paragraph_builder.cc`.


def patch_text_encoding(text):
    changed = False

    # --- TextStyle encoder -------------------------------------------------
    text, c = replace_once(
        text,
        "  double? wordSpacing,\n  double? height,\n  Locale? locale,\n  Paint? background,",
        "  double? wordSpacing,\n"
        "  double? height,\n"
        "  double? lineHeight,\n"
        "  Locale? locale,\n"
        "  Paint? background,",
        "TextStyle encoder lineHeight",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  List<FontVariation>? fontVariations,\n"
        ") {\n"
        "  final Int32List result = Int32List(9);",
        "  List<FontVariation>? fontVariations,\n"
        "  bool ignoreClusterLetterSpacing,\n"
        "  bool useCoreTextWordBreak,\n"
        ") {\n"
        "  final Int32List result = Int32List(9);",
        "TextStyle encoder flags",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  if (fontVariations != null) {\n"
        "    result[0] |= 1 << 19;\n"
        "    // Passed separately to native.\n"
        "  }\n"
        "\n"
        "  return result;\n"
        "}",
        "  if (fontVariations != null) {\n"
        "    result[0] |= 1 << 19;\n"
        "    // Passed separately to native.\n"
        "  }\n"
        "  if (lineHeight != null) {\n"
        "    result[0] |= 1 << 20;\n"
        "  }\n"
        "  if (ignoreClusterLetterSpacing != false) {\n"
        "    result[0] |= 1 << 21;\n"
        "  }\n"
        "  if (useCoreTextWordBreak != false) {\n"
        "    result[0] |= 1 << 22;\n"
        "  }\n"
        "\n"
        "  return result;\n"
        "}",
        "TextStyle encoder bits",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "         wordSpacing,\n"
        "         height,\n"
        "         locale,",
        "         wordSpacing,\n"
        "         height,\n"
        "         lineHeight,\n"
        "         locale,",
        "TextStyle encoder lineHeight call",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "         fontVariations,\n"
        "       ),",
        "         fontVariations,\n"
        "         ignoreClusterLetterSpacing,\n"
        "         useCoreTextWordBreak,\n"
        "       ),",
        "TextStyle encoder call",
    )
    changed = changed or c

    # --- ParagraphStyle encoder -------------------------------------------
    text, c = replace_once(
        text,
        "  double? height,\n  TextHeightBehavior? textHeightBehavior,",
        "  double? height,\n"
        "  double? lineHeight,\n"
        "  TextHeightBehavior? textHeightBehavior,",
        "ParagraphStyle encoder lineHeight",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  StrutStyle? strutStyle,\n"
        "  String? ellipsis,\n"
        "  Locale? locale,\n"
        ") {\n"
        "  final Int32List result = Int32List(7);",
        "  StrutStyle? strutStyle,\n"
        "  String? ellipsis,\n"
        "  Locale? locale,\n"
        "  bool ignoreClusterLetterSpacing,\n"
        "  bool backgroundUseLineMetrics,\n"
        ") {\n"
        "  final Int32List result = Int32List(7);",
        "ParagraphStyle encoder flags",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  if (locale != null) {\n"
        "    result[0] |= 1 << 12;\n"
        "    // Passed separately to native.\n"
        "  }\n"
        "  return result;\n"
        "}",
        "  if (locale != null) {\n"
        "    result[0] |= 1 << 12;\n"
        "    // Passed separately to native.\n"
        "  }\n"
        "  if (lineHeight != null) {\n"
        "    result[0] |= 1 << 13;\n"
        "  }\n"
        "  if (ignoreClusterLetterSpacing) {\n"
        "    result[0] |= 1 << 14;\n"
        "  }\n"
        "  if (backgroundUseLineMetrics) {\n"
        "    result[0] |= 1 << 15;\n"
        "  }\n"
        "  return result;\n"
        "}",
        "ParagraphStyle encoder bits",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "         fontSize,\n"
        "         height,\n"
        "         textHeightBehavior,",
        "         fontSize,\n"
        "         height,\n"
        "         lineHeight,\n"
        "         textHeightBehavior,",
        "ParagraphStyle encoder lineHeight call",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "         strutStyle,\n"
        "         ellipsis,\n"
        "         locale,\n"
        "       ),",
        "         strutStyle,\n"
        "         ellipsis,\n"
        "         locale,\n"
        "         ignoreClusterLetterSpacing,\n"
        "         backgroundUseLineMetrics,\n"
        "       ),",
        "ParagraphStyle encoder call",
    )
    changed = changed or c

    # --- ParagraphBuilder native calls -------------------------------------
    text, c = replace_once(
        text,
        "      style._fontSize ?? 0,\n"
        "      style._height ?? 0,\n"
        "      style._ellipsis ?? '',",
        "      style._fontSize ?? 0,\n"
        "      style._height ?? 0,\n"
        "      style._lineHeight ?? 0,\n"
        "      style._ellipsis ?? '',",
        "ParagraphBuilder::Create lineHeight argument",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  @Native<Void Function(Handle, Handle, Handle, Handle, Handle, Double, Double, Handle, Handle)>(",
        "  @Native<Void Function(Handle, Handle, Handle, Handle, Handle, Double, Double, Double, Handle, Handle)>(",
        "ParagraphBuilder::Create native signature",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    double height,\n"
        "    String ellipsis,",
        "    double height,\n"
        "    double lineHeight,\n"
        "    String ellipsis,",
        "ParagraphBuilder::Create declaration",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "      style._wordSpacing ?? 0,\n"
        "      style._height ?? 0,\n"
        "      style._decorationThickness ?? 0,",
        "      style._wordSpacing ?? 0,\n"
        "      style._height ?? 0,\n"
        "      style._lineHeight ?? 0,\n"
        "      style._decorationThickness ?? 0,",
        "ParagraphBuilder::pushStyle lineHeight argument",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "      Pointer<Void>,\n"
        "      Handle,\n"
        "      Handle,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Handle,",
        "      Pointer<Void>,\n"
        "      Handle,\n"
        "      Handle,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Double,\n"
        "      Handle,",
        "ParagraphBuilder::pushStyle native signature",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    double wordSpacing,\n"
        "    double height,\n"
        "    double decorationThickness,",
        "    double wordSpacing,\n"
        "    double height,\n"
        "    double lineHeight,\n"
        "    double decorationThickness,",
        "ParagraphBuilder::pushStyle declaration",
    )
    changed = changed or c

    # `_encoded` only records that lineHeight is present; the value itself
    # participates in equality like the recovered private source.
    text, c = replace_once(
        text,
        "        other._height == _height &&\n"
        "        other._decorationThickness == _decorationThickness &&",
        "        other._height == _height &&\n"
        "        other._lineHeight == _lineHeight &&\n"
        "        other._decorationThickness == _decorationThickness &&",
        "TextStyle lineHeight equality",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "        other._height == _height &&\n"
        "        other._strutStyle == _strutStyle &&",
        "        other._height == _height &&\n"
        "        other._lineHeight == _lineHeight &&\n"
        "        other._strutStyle == _strutStyle &&",
        "ParagraphStyle lineHeight equality",
    )
    changed = changed or c

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


# ---------------------------------------------------------------------------
# Native text implementation: decode the private flags
# ---------------------------------------------------------------------------
#
# `patch_text_encoding` above makes `dart:ui` emit the private bits; the files
# below are the Linux/Windows text backend standing in for the private
# engine's `skyline_ext` text implementation.  They decode the flags into the
# `txt` style structs and apply them in SkParagraph:
#
#   * backgroundUseLineMetrics: paint span backgrounds and report selection
#     boxes using the full line box instead of the run's font metrics.
#   * useCoreTextWordBreak: carried into the SkParagraph text style for
#     CoreText-based word breakers (iOS/macOS).  Linux/Windows keep the ICU
#     line breaker, matching the official Windows build.
#   * lineHeight: absolute CSS line height (px); converted to a `height`
#     multiplier when the native style has a font size.
#   * ignoreClusterLetterSpacing: carried into the SkParagraph text style so
#     the upstream/private shaping code can use it.


def patch_paragraph_builder_h(text):
    text, c1 = replace_once(
        text,
        "                     double fontSize,\n"
        "                     double height,\n"
        "                     const std::u16string& ellipsis,",
        "                     double fontSize,\n"
        "                     double height,\n"
        "                     double lineHeight,\n"
        "                     const std::u16string& ellipsis,",
        "ParagraphBuilder::Create header lineHeight",
    )
    text, c2 = replace_once(
        text,
        "                 double height,\n"
        "                 double decorationThickness,",
        "                 double height,\n"
        "                 double lineHeight,\n"
        "                 double decorationThickness,",
        "ParagraphBuilder::pushStyle header lineHeight",
    )
    text, c3 = replace_once(
        text,
        "                            double height,\n"
        "                            const std::u16string& ellipsis,",
        "                            double height,\n"
        "                            double lineHeight,\n"
        "                            const std::u16string& ellipsis,",
        "ParagraphBuilder ctor header lineHeight",
    )
    return text, c1 or c2 or c3


def patch_paragraph_builder_cc(text):
    changed = False

    text, c = replace_once(
        text,
        "const int kPSLocaleMask = 1 << kPSLocaleIndex;",
        "const int kPSLocaleMask = 1 << kPSLocaleIndex;\n"
        "\n"
        "// Private Skyline additions (encoded by `_encodeParagraphStyle`).\n"
        "const int kPSLineHeightIndex = 13;\n"
        "const int kPSIgnoreClusterLetterSpacingIndex = 14;\n"
        "const int kPSBackgroundUseLineMetricsIndex = 15;\n"
        "\n"
        "const int kPSLineHeightMask = 1 << kPSLineHeightIndex;\n"
        "const int kPSIgnoreClusterLetterSpacingMask =\n"
        "    1 << kPSIgnoreClusterLetterSpacingIndex;\n"
        "const int kPSBackgroundUseLineMetricsMask =\n"
        "    1 << kPSBackgroundUseLineMetricsIndex;",
        "paragraph style flag masks",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "const int kTSFontVariationsMask = 1 << kTSFontVariationsIndex;",
        "const int kTSFontVariationsMask = 1 << kTSFontVariationsIndex;\n"
        "\n"
        "// Private Skyline additions (encoded by `_encodeTextStyle`).\n"
        "const int kTSLineHeightIndex = 20;\n"
        "const int kTSIgnoreClusterLetterSpacingIndex = 21;\n"
        "const int kTSUseCoreTextWordBreakIndex = 22;\n"
        "\n"
        "const int kTSLineHeightMask = 1 << kTSLineHeightIndex;\n"
        "const int kTSIgnoreClusterLetterSpacingMask =\n"
        "    1 << kTSIgnoreClusterLetterSpacingIndex;\n"
        "const int kTSUseCoreTextWordBreakMask = 1 << kTSUseCoreTextWordBreakIndex;",
        "text style flag masks",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "                              double fontSize,\n"
        "                              double height,\n"
        "                              const std::u16string& ellipsis,\n"
        "                              const std::string& locale) {\n"
        "  UIDartState::ThrowIfUIOperationsProhibited();\n"
        "  auto res = fml::MakeRefCounted<ParagraphBuilder>(\n"
        "      encoded_handle, strutData, fontFamily, strutFontFamilies, fontSize,\n"
        "      height, ellipsis, locale);",
        "                              double fontSize,\n"
        "                              double height,\n"
        "                              double lineHeight,\n"
        "                              const std::u16string& ellipsis,\n"
        "                              const std::string& locale) {\n"
        "  UIDartState::ThrowIfUIOperationsProhibited();\n"
        "  auto res = fml::MakeRefCounted<ParagraphBuilder>(\n"
        "      encoded_handle, strutData, fontFamily, strutFontFamilies, fontSize,\n"
        "      height, lineHeight, ellipsis, locale);",
        "ParagraphBuilder::Create lineHeight",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    double fontSize,\n"
        "    double height,\n"
        "    const std::u16string& ellipsis,\n"
        "    const std::string& locale) {\n"
        "  int32_t mask = 0;",
        "    double fontSize,\n"
        "    double height,\n"
        "    double lineHeight,\n"
        "    const std::u16string& ellipsis,\n"
        "    const std::string& locale) {\n"
        "  int32_t mask = 0;",
        "ParagraphBuilder ctor lineHeight",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    if (mask & kPSMaxLinesMask) {\n"
        "      style.max_lines = encoded[kPSMaxLinesIndex];\n"
        "    }\n"
        "  }",
        "    if (mask & kPSMaxLinesMask) {\n"
        "      style.max_lines = encoded[kPSMaxLinesIndex];\n"
        "    }\n"
        "\n"
        "    if (mask & kPSLineHeightMask) {\n"
        "      style.line_height = lineHeight;\n"
        "    }\n"
        "    style.ignore_cluster_letter_spacing =\n"
        "        (mask & kPSIgnoreClusterLetterSpacingMask) != 0;\n"
        "    style.background_use_line_metrics =\n"
        "        (mask & kPSBackgroundUseLineMetricsMask) != 0;\n"
        "  }",
        "paragraph style flag decode",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "                                 double wordSpacing,\n"
        "                                 double height,\n"
        "                                 double decorationThickness,",
        "                                 double wordSpacing,\n"
        "                                 double height,\n"
        "                                 double lineHeight,\n"
        "                                 double decorationThickness,",
        "ParagraphBuilder::pushStyle lineHeight",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  if (mask & kTSHeightMask) {\n"
        "    style.height = height;\n"
        "    style.has_height_override = style.height != kTextHeightNone;\n"
        "  }",
        "  if (mask & kTSHeightMask) {\n"
        "    style.height = height;\n"
        "    style.has_height_override = style.height != kTextHeightNone;\n"
        "  }\n"
        "\n"
        "  if (mask & kTSLineHeightMask) {\n"
        "    style.line_height = lineHeight;\n"
        "  }\n"
        "  style.ignore_cluster_letter_spacing =\n"
        "      (mask & kTSIgnoreClusterLetterSpacingMask) != 0;\n"
        "  style.use_core_text_word_break =\n"
        "      (mask & kTSUseCoreTextWordBreakMask) != 0;",
        "text style flag decode",
    )
    changed = changed or c

    return text, changed


def patch_txt_text_style_h(text):
    text, c = replace_once(
        text,
        "  double height = 1.0;\n"
        "  bool has_height_override = false;",
        "  double height = 1.0;\n"
        "  bool has_height_override = false;\n"
        "  // Private Skyline additions.\n"
        "  double line_height = 0.0;\n"
        "  bool ignore_cluster_letter_spacing = false;\n"
        "  bool use_core_text_word_break = false;",
        "txt::TextStyle flag fields",
    )
    return text, c


def patch_txt_text_style_cc(text):
    text, c = replace_once(
        text,
        "  if (half_leading != other.half_leading) {\n"
        "    return false;\n"
        "  }",
        "  if (half_leading != other.half_leading) {\n"
        "    return false;\n"
        "  }\n"
        "  if (line_height != other.line_height ||\n"
        "      ignore_cluster_letter_spacing != other.ignore_cluster_letter_spacing ||\n"
        "      use_core_text_word_break != other.use_core_text_word_break) {\n"
        "    return false;\n"
        "  }",
        "txt::TextStyle equality",
    )
    return text, c


def patch_txt_paragraph_style_h(text):
    text, c = replace_once(
        text,
        "  double height = 1;\n"
        "  bool has_height_override = false;",
        "  double height = 1;\n"
        "  bool has_height_override = false;\n"
        "  // Private Skyline additions.\n"
        "  double line_height = 0;\n"
        "  bool ignore_cluster_letter_spacing = false;\n"
        "  bool background_use_line_metrics = false;",
        "txt::ParagraphStyle flag fields",
    )
    return text, c


def patch_txt_paragraph_style_cc(text):
    text, c = replace_once(
        text,
        "  result.height = height;\n"
        "  result.has_height_override = has_height_override;\n"
        "  return result;",
        "  result.height = height;\n"
        "  result.has_height_override = has_height_override;\n"
        "  result.line_height = line_height;\n"
        "  result.ignore_cluster_letter_spacing = ignore_cluster_letter_spacing;\n"
        "  return result;",
        "txt::ParagraphStyle::GetTextStyle flags",
    )
    return text, c


def patch_paragraph_builder_skia(text):
    changed = False

    text, c = replace_once(
        text,
        "  text_style.setHeight(SkDoubleToScalar(txt.height));\n"
        "  text_style.setHeightOverride(txt.has_height_override);",
        "  // Skyline's lineHeight is an absolute px value; SkParagraph consumes a\n"
        "  // multiplier of the font size.  Prefer it when both are present.\n"
        "  if (txt.line_height > 0 && txt.font_size > 0) {\n"
        "    text_style.setHeight(\n"
        "        SkDoubleToScalar(txt.line_height / txt.font_size));\n"
        "    text_style.setHeightOverride(true);\n"
        "  } else {\n"
        "    text_style.setHeight(SkDoubleToScalar(txt.height));\n"
        "    text_style.setHeightOverride(txt.has_height_override);\n"
        "  }\n"
        "  text_style.setIgnoreClusterLetterSpacing(\n"
        "      txt.ignore_cluster_letter_spacing);",
        "paragraph default text style lineHeight",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  skia.setTextHeightBehavior(\n"
        "      static_cast<skt::TextHeightBehavior>(txt.text_height_behavior));",
        "  skia.setTextHeightBehavior(\n"
        "      static_cast<skt::TextHeightBehavior>(txt.text_height_behavior));\n"
        "  skia.setBackgroundUseLineMetrics(txt.background_use_line_metrics);",
        "skia paragraph style flag",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "  skia.setHeight(SkDoubleToScalar(txt.height));\n"
        "  skia.setHeightOverride(txt.has_height_override);\n"
        "  skia.setHalfLeading(txt.half_leading);",
        "  if (txt.line_height > 0 && txt.font_size > 0) {\n"
        "    skia.setHeight(SkDoubleToScalar(txt.line_height / txt.font_size));\n"
        "    skia.setHeightOverride(true);\n"
        "  } else {\n"
        "    skia.setHeight(SkDoubleToScalar(txt.height));\n"
        "    skia.setHeightOverride(txt.has_height_override);\n"
        "  }\n"
        "  skia.setHalfLeading(txt.half_leading);\n"
        "  skia.setIgnoreClusterLetterSpacing(txt.ignore_cluster_letter_spacing);\n"
        "  skia.setUseCoreTextWordBreak(txt.use_core_text_word_break);",
        "span text style flags",
    )
    changed = changed or c

    return text, changed


def patch_skia_text_style_h(text):
    changed = False

    text, c = replace_once(
        text,
        "    void setHalfLeading(bool halfLeading) { fHalfLeading = halfLeading; }\n"
        "    bool getHalfLeading() const { return fHalfLeading; }",
        "    void setHalfLeading(bool halfLeading) { fHalfLeading = halfLeading; }\n"
        "    bool getHalfLeading() const { return fHalfLeading; }\n"
        "\n"
        "    // Private Skyline additions.\n"
        "    void setIgnoreClusterLetterSpacing(bool value) {\n"
        "        fIgnoreClusterLetterSpacing = value;\n"
        "    }\n"
        "    bool getIgnoreClusterLetterSpacing() const {\n"
        "        return fIgnoreClusterLetterSpacing;\n"
        "    }\n"
        "    void setUseCoreTextWordBreak(bool value) {\n"
        "        fUseCoreTextWordBreak = value;\n"
        "    }\n"
        "    bool getUseCoreTextWordBreak() const { return fUseCoreTextWordBreak; }",
        "skia TextStyle flag accessors",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    // true: half leading.\n"
        "    // false: scale ascent/descent with fHeight.\n"
        "    bool fHalfLeading = false;",
        "    // true: half leading.\n"
        "    // false: scale ascent/descent with fHeight.\n"
        "    bool fHalfLeading = false;\n"
        "    // Private Skyline additions.\n"
        "    bool fIgnoreClusterLetterSpacing = false;\n"
        "    bool fUseCoreTextWordBreak = false;",
        "skia TextStyle flag fields",
    )
    changed = changed or c

    return text, changed


def patch_skia_text_style_cc(text):
    changed = False

    text, c = replace_once(
        text,
        "    if (fHalfLeading != other.fHalfLeading) {\n"
        "        return false;\n"
        "    }",
        "    if (fHalfLeading != other.fHalfLeading) {\n"
        "        return false;\n"
        "    }\n"
        "    if (fIgnoreClusterLetterSpacing != other.fIgnoreClusterLetterSpacing ||\n"
        "        fUseCoreTextWordBreak != other.fUseCoreTextWordBreak) {\n"
        "        return false;\n"
        "    }",
        "skia TextStyle equality",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "           nearlyEqual(fFontSize, that.fFontSize) &&\n"
        "           fLocale == that.fLocale;",
        "           nearlyEqual(fFontSize, that.fFontSize) &&\n"
        "           fIgnoreClusterLetterSpacing == that.fIgnoreClusterLetterSpacing &&\n"
        "           fUseCoreTextWordBreak == that.fUseCoreTextWordBreak &&\n"
        "           fLocale == that.fLocale;",
        "skia TextStyle font equality",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "                   fHalfLeading == other.fHalfLeading &&\n"
        "                   fBaselineShift == other.fBaselineShift &&",
        "                   fHalfLeading == other.fHalfLeading &&\n"
        "                   fIgnoreClusterLetterSpacing == other.fIgnoreClusterLetterSpacing &&\n"
        "                   fUseCoreTextWordBreak == other.fUseCoreTextWordBreak &&\n"
        "                   fBaselineShift == other.fBaselineShift &&",
        "skia TextStyle font attribute",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    result.fHalfLeading = fHalfLeading;\n"
        "    result.fBaselineShift = fBaselineShift;",
        "    result.fHalfLeading = fHalfLeading;\n"
        "    result.fIgnoreClusterLetterSpacing = fIgnoreClusterLetterSpacing;\n"
        "    result.fUseCoreTextWordBreak = fUseCoreTextWordBreak;\n"
        "    result.fBaselineShift = fBaselineShift;",
        "skia TextStyle placeholder clone",
    )
    changed = changed or c

    return text, changed


def patch_skia_paragraph_style_h(text):
    changed = False

    text, c = replace_once(
        text,
        "               this->fDefaultTextStyle == rhs.fDefaultTextStyle &&\n"
        "               this->fReplaceTabCharacters == rhs.fReplaceTabCharacters;",
        "               this->fDefaultTextStyle == rhs.fDefaultTextStyle &&\n"
        "               this->fBackgroundUseLineMetrics == rhs.fBackgroundUseLineMetrics &&\n"
        "               this->fReplaceTabCharacters == rhs.fReplaceTabCharacters;",
        "skia ParagraphStyle equality",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    bool getApplyRoundingHack() const { return fApplyRoundingHack; }\n"
        "    void setApplyRoundingHack(bool value) { fApplyRoundingHack = value; }",
        "    bool getApplyRoundingHack() const { return fApplyRoundingHack; }\n"
        "    void setApplyRoundingHack(bool value) { fApplyRoundingHack = value; }\n"
        "\n"
        "    // Private Skyline addition: span backgrounds and selection boxes use\n"
        "    // the full line box instead of the run's font metrics.\n"
        "    bool getBackgroundUseLineMetrics() const {\n"
        "        return fBackgroundUseLineMetrics;\n"
        "    }\n"
        "    void setBackgroundUseLineMetrics(bool value) {\n"
        "        fBackgroundUseLineMetrics = value;\n"
        "    }",
        "skia ParagraphStyle accessors",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    bool fReplaceTabCharacters;\n"
        "    bool fApplyRoundingHack = true;",
        "    bool fReplaceTabCharacters;\n"
        "    bool fBackgroundUseLineMetrics = false;\n"
        "    bool fApplyRoundingHack = true;",
        "skia ParagraphStyle field",
    )
    changed = changed or c

    return text, changed


def patch_paragraph_cache(text):
    changed = False

    text, c = replace_once(
        text,
        "    hash = mix(hash, SkGoodHash()(fParagraphStyle.getReplaceTabCharacters() ? 1 : 0));",
        "    hash = mix(hash, SkGoodHash()(fParagraphStyle.getReplaceTabCharacters() ? 1 : 0));\n"
        "    hash = mix(hash, SkGoodHash()(fParagraphStyle.getBackgroundUseLineMetrics() ? 1 : 0));",
        "ParagraphCache flag hash",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "    if (!(fParagraphStyle.getReplaceTabCharacters() == other.fParagraphStyle.getReplaceTabCharacters())) {\n"
        "        return false;\n"
        "    }",
        "    if (!(fParagraphStyle.getReplaceTabCharacters() == other.fParagraphStyle.getReplaceTabCharacters())) {\n"
        "        return false;\n"
        "    }\n"
        "\n"
        "    if (fParagraphStyle.getBackgroundUseLineMetrics() != other.fParagraphStyle.getBackgroundUseLineMetrics()) {\n"
        "        return false;\n"
        "    }",
        "ParagraphCache flag equality",
    )
    changed = changed or c

    return text, changed


def patch_text_line(text):
    changed = False

    text, c = replace_once(
        text,
        "    if (style.hasBackground()) {\n"
        "        painter->drawRect(context.clip.makeOffset(this->offset() + SkPoint::Make(x, y)),\n"
        "                          style.getBackgroundPaintOrID());\n"
        "    }",
        "    if (style.hasBackground()) {\n"
        "        SkRect rect = context.clip;\n"
        "        if (fOwner->paragraphStyle().getBackgroundUseLineMetrics()) {\n"
        "            // Use the full line box (including strut and leading) instead\n"
        "            // of this run's font metrics.\n"
        "            rect.fTop = 0.0f;\n"
        "            rect.fBottom = this->height();\n"
        "        }\n"
        "        painter->drawRect(rect.makeOffset(this->offset() + SkPoint::Make(x, y)),\n"
        "                          style.getBackgroundPaintOrID());\n"
        "    }",
        "TextLine background line metrics",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "            auto paragraphStyle = fOwner->paragraphStyle();\n"
        "\n"
        "            // Found a run that intersects with the text",
        "            auto paragraphStyle = fOwner->paragraphStyle();\n"
        "            RectHeightStyle effectiveRectHeightStyle = rectHeightStyle;\n"
        "            if (paragraphStyle.getBackgroundUseLineMetrics()) {\n"
        "                // Selection boxes follow the line box as well.\n"
        "                effectiveRectHeightStyle = RectHeightStyle::kMax;\n"
        "            }\n"
        "\n"
        "            // Found a run that intersects with the text",
        "TextLine selection line metrics",
    )
    changed = changed or c

    text, c = replace_once(
        text,
        "            switch (rectHeightStyle) {\n"
        "                case RectHeightStyle::kMax:\n"
        "                    // TODO: Change it once flutter rolls into google3",
        "            switch (effectiveRectHeightStyle) {\n"
        "                case RectHeightStyle::kMax:\n"
        "                    // TODO: Change it once flutter rolls into google3",
        "TextLine selection height switch",
    )
    changed = changed or c

    return text, changed


def main():
    edit("flutter/lib/ui/ui.dart", patch_ui)
    edit("flutter/lib/ui/painting.dart", patch_painting)
    edit("flutter/lib/ui/platform_dispatcher.dart", patch_platform_dispatcher)
    edit("flutter/lib/ui/window.dart", patch_window)
    edit("flutter/lib/ui/painting.dart", patch_canvas)
    edit("flutter/lib/ui/text.dart", patch_text)
    edit("flutter/lib/ui/text/paragraph_builder.h", patch_paragraph_builder_h)
    edit("flutter/lib/ui/text/paragraph_builder.cc", patch_paragraph_builder_cc)
    edit("flutter/txt/src/txt/text_style.h", patch_txt_text_style_h)
    edit("flutter/txt/src/txt/text_style.cc", patch_txt_text_style_cc)
    edit("flutter/txt/src/txt/paragraph_style.h", patch_txt_paragraph_style_h)
    edit("flutter/txt/src/txt/paragraph_style.cc", patch_txt_paragraph_style_cc)
    edit("flutter/txt/src/skia/paragraph_builder_skia.cc", patch_paragraph_builder_skia)
    edit("flutter/third_party/skia/modules/skparagraph/include/TextStyle.h", patch_skia_text_style_h)
    edit("flutter/third_party/skia/modules/skparagraph/src/TextStyle.cpp", patch_skia_text_style_cc)
    edit("flutter/third_party/skia/modules/skparagraph/include/ParagraphStyle.h", patch_skia_paragraph_style_h)
    edit("flutter/third_party/skia/modules/skparagraph/src/ParagraphCache.cpp", patch_paragraph_cache)
    edit("flutter/third_party/skia/modules/skparagraph/src/TextLine.cpp", patch_text_line)
    edit("flutter/third_party/dart/sdk/lib/io/platform.dart", patch_io_platform)
    return 0


if __name__ == "__main__":
    sys.exit(main())

// Skyline compatibility surface for the public `dart:ui`.
//
// The private engine embedded in `skyline.node` carries a `skyline_ext` overlay
// on top of Flutter's `dart:ui`.  `package:skyline` (and the patched framework
// sources shipped in the official kernel) use those additions.  This part
// re-declares the additive surface needed for compilation against the public
// engine; implementation notes call out where behaviour is intentionally a
// no-op until the corresponding native path is ported.
//
// Sources recovered from the official `isolate_snapshot_data`:
//   * `window.dart` -> WindowStatus
//   * `painting.dart` -> createNativeParagraph(), warmUpFont()
//   * `compositing.dart` -> SceneBuilder checkerboard toggles
//
// Other additive members (PlatformDispatcher refresh rate/GC/thresholds,
// TextStyle.lineHeight, ...) are patched directly into their owning classes by
// `native/engine/dart_ui_compat/apply.py`.
//
// Keep this file in sync with `spec/kernel-symbols.json`
// (`dartUiCustomMembers`, `engineNativeSymbols`).

part of dart.ui;

/// Represents the status of the window corresponding to a given [FlutterView].
///
/// This is populated only on Android and Ohos in the official engine.  The
/// public engine always reports [WindowStatus.normal].
enum WindowStatus {
  /// The window status is normal.
  normal,

  /// The window is floating.
  floating,

  /// On Android, the window is in multi-window mode.
  /// On Ohos, the window is in SPLIT_SCREEN status.
  splitScreen,
}

/// Signature for `PlatformDispatcher.onReportSlowRunnerStack`.
typedef SlowRunnerStackCallback = void Function(
  String name,
  int milliseconds,
  List<String> stackTraces,
);

/// Debug toggles the private engine adds to [SceneBuilder].
extension SkylineSceneBuilderCompat on SceneBuilder {
  /// Toggles the raster-cache checkerboard debug overlay.
  ///
  /// The public engine has no `SceneBuilder::setCheckerboard*` hooks; keep the
  /// API surface as a no-op.
  void setCheckerboardRasterCacheImages(bool checkerboard) {}

  /// Toggles partial-paint-area checkerboarding.
  ///
  /// The public engine has no `SceneBuilder::setCheckerboard*` hooks; keep the
  /// API surface as a no-op.
  void setCheckerboardPartialPaintAreas(bool checkerboard) {}
}

/// Creates an uninitialized native paragraph.
///
/// `package:skyline` passes the returned object to its FFI callback
/// (`createSkylineParagraph`), which fills it from the Skyline native runtime.
Paragraph createNativeParagraph() {
  return _NativeParagraph._();
}

/// Warms up the font cache.
///
/// The private engine implements this with
/// `@Native(symbol: 'FontWarmUp::WarmUp')`.  The public engine has no such
/// symbol, so the compatibility layer keeps the API surface as a no-op until
/// the font warm-up path is ported.
void warmUpFont(String cacheDir) {
  // no-op compatibility stub
}

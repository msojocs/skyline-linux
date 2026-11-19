// Skyline compatibility additions for `dart:ui` (originally in painting.dart).
//
// These are appended to `flutter/lib/ui/painting.dart` by
// `native/engine/patch-dart-ui.sh`.

// NOTE: added by skyline.
//
// Returns an uninitialized native paragraph.  `package:skyline` fills it from
// the native runtime through its FFI callback (`createSkylineParagraph`).
Paragraph createNativeParagraph() {
  return _NativeParagraph._();
}

// NOTE: added by skyline.
//
// The official private engine declares this as:
//
//   @Native<Void Function(Handle)>(symbol: 'FontWarmUp::WarmUp')
//   external void warmUpFont(String cacheDir);
//
// The public engine does not implement `FontWarmUp::WarmUp`, so this compat
// layer keeps the API surface as a no-op.  Wire it to the native symbol once
// the font warm-up path is ported.
void warmUpFont(String cacheDir) {
  // no-op compatibility stub
}

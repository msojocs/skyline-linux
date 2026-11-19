// Minimal pure `dart:ui` application used by the phase-2 engine smoke test.
//
// It deliberately avoids `package:flutter`: the toolchain only downloads the Dart
// SDK + flutter_patched_sdk (no full Flutter SDK), and the host only needs to prove
// that the engine can boot an isolate, produce a frame, and hand the pixels back.
//
// The page is a single opaque red rectangle.  The smoke test uses the known color to
// verify both the frame size and the BGRA->RGBA conversion performed by the host.

import 'dart:ui' as ui;

const ui.Color _pageColor = ui.Color(0xFFFF0000); // opaque red

bool _rendered = false;

void main() {
  final ui.PlatformDispatcher dispatcher = ui.PlatformDispatcher.instance;

  dispatcher.onMetricsChanged = () {
    if (!_rendered) dispatcher.scheduleFrame();
  };

  dispatcher.onBeginFrame = (Duration _) {};

  dispatcher.onDrawFrame = () {
    final ui.FlutterView? view = dispatcher.implicitView;
    final ui.Size? size = view?.physicalSize;
    if (view == null || size == null || size.isEmpty) {
      // Metrics have not arrived yet; the onMetricsChanged callback reschedules.
      dispatcher.scheduleFrame();
      return;
    }

    final ui.PictureRecorder recorder = ui.PictureRecorder();
    final ui.Rect rect = ui.Offset.zero & size;
    final ui.Canvas canvas = ui.Canvas(recorder, rect);
    canvas.drawRect(rect, ui.Paint()..color = _pageColor);
    final ui.Picture picture = recorder.endRecording();

    final ui.SceneBuilder builder = ui.SceneBuilder()
      ..addPicture(ui.Offset.zero, picture);
    final ui.Scene scene = builder.build();
    view.render(scene);

    scene.dispose();
    picture.dispose();
    _rendered = true;
  };

  dispatcher.scheduleFrame();
}

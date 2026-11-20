// Runtime smoke test for the Skyline `dart:ui` compatibility layer.
//
// It touches every additive member that `package:skyline` relies on, renders
// two paragraphs that exercise the private text flags, and then fills a solid
// green frame, so the engine smoke test can assert that the patched core
// snapshot was loaded (not just that the app kernel compiled).
//
// Compile against the patched SDK:
//   SKYLINE_APP_DIR=apps/dart-ui-compat-smoke \
//     bash native/engine/build-empty-page.sh --patched-sdk

import 'dart:ui' as ui;

const ui.Color _pageColor = ui.Color(0xFF00FF00); // opaque green

bool _rendered = false;

void main() {
  final ui.Application app = ui.Application.current;
  app.put('smoke', 42);
  final int? stored = app.find<int>('smoke');
  final String created = app.get<String>('created', () => 'yes');
  final double refreshRate = ui.PlatformDispatcher.instance.getDisplayRefreshRate();
  final bool parallel = ui.PlatformDispatcher.isUseParallelPipeline();
  ui.PlatformDispatcher.setUseParallelPipeline(parallel);
  ui.PlatformDispatcher.instance.setUiRunnerThreshold(0);
  ui.PlatformDispatcher.instance.setRasterRunnerThreshold(0);
  ui.PlatformDispatcher.instance.onReportSlowRunnerStack =
      (String name, int milliseconds, List<String> stackTraces) {};
  ui.PlatformDispatcher.instance.collectGarbage();
  ui.warmUpFont('/tmp');

  final ui.Paragraph paragraph = ui.createNativeParagraph();
  final ui.WindowStatus status = ui.WindowStatus.normal;
  final ui.FrameTiming timing = ui.FrameTiming(
    vsyncStart: 11,
    buildStart: 12,
    buildFinish: 13,
    rasterStart: 14,
    rasterFinish: 15,
    rasterFinishWallTime: 16,
  );
  final int vsyncTarget = timing.timestampInMicroseconds(ui.FramePhase.vsyncTarget);
  // ignore: avoid_print
  print(
    'compat-smoke id=${app.id} default=${app.isDefaultApplication()} '
    'stored=$stored created=$created refreshRate=$refreshRate parallel=$parallel '
    'status=$status paragraph=${paragraph.hashCode} vsyncTarget=$vsyncTarget',
  );

  final ui.PlatformDispatcher dispatcher = ui.PlatformDispatcher.instance;

  dispatcher.onMetricsChanged = () {
    if (!_rendered) dispatcher.scheduleFrame();
  };

  dispatcher.onBeginFrame = (Duration _) {};

  dispatcher.onDrawFrame = () {
    final ui.FlutterView? view = dispatcher.implicitView;
    final ui.Size? size = view?.physicalSize;
    if (view == null || size == null || size.isEmpty) {
      dispatcher.scheduleFrame();
      return;
    }

    // Also touch the window-status getter once a view exists.
    // ignore: avoid_print
    print('compat-smoke windowStatus=${view.windowStatus}');

    final ui.PictureRecorder recorder = ui.PictureRecorder();
    final ui.Rect rect = ui.Offset.zero & size;
    final ui.Canvas canvas = ui.Canvas(recorder, rect);
    // Exercise the private `ClipOp` parameter without clipping the whole frame
    // (a rounded corner would anti-alias the first pixel).
    canvas.save();
    canvas.clipRRect(
      ui.RRect.fromRectAndRadius(rect, const ui.Radius.circular(1)),
      clipOp: ui.ClipOp.intersect,
    );
    canvas.restore();
    canvas.drawRect(rect, ui.Paint()..color = _pageColor);

    // Exercise the private text flags end to end.  The strut forces a 40px line
    // box while the run is 10px tall, so only `backgroundUseLineMetrics` paints
    // the span background across the full line height.
    final ui.Paint textBackground = ui.Paint()..color = const ui.Color(0xFF0000FF);
    void drawFlaggedParagraph(bool useLineMetrics, double x) {
      final ui.ParagraphStyle style = ui.ParagraphStyle(
        textDirection: ui.TextDirection.ltr,
        backgroundUseLineMetrics: useLineMetrics,
        strutStyle: ui.StrutStyle(
          fontSize: 20,
          height: 2,
          forceStrutHeight: true,
        ),
      );
      final ui.TextStyle textStyle = ui.TextStyle(
        color: const ui.Color(0xFF000000),
        background: textBackground,
        fontSize: 10,
        useCoreTextWordBreak: true,
        ignoreClusterLetterSpacing: true,
      );
      final ui.ParagraphBuilder builder = ui.ParagraphBuilder(style)
        ..pushStyle(textStyle)
        ..addText('I');
      final ui.Paragraph paragraph = builder.build()
        ..layout(const ui.ParagraphConstraints(width: 24));
      canvas.drawParagraph(paragraph, ui.Offset(x, 8));
      paragraph.dispose();
    }

    drawFlaggedParagraph(true, 8);
    drawFlaggedParagraph(false, 40);
    final ui.Picture picture = recorder.endRecording();

    final ui.SceneBuilder builder = ui.SceneBuilder()
      ..setCheckerboardRasterCacheImages(false)
      ..setCheckerboardPartialPaintAreas(false)
      ..addPicture(ui.Offset.zero, picture);
    final ui.Scene scene = builder.build();
    view.render(scene);

    scene.dispose();
    picture.dispose();
    _rendered = true;
  };

  dispatcher.scheduleFrame();
}

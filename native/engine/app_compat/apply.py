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

# Native-view hit testing: plain YGNode render objects do not forward hit tests
# to their single child, so the texture widget's RenderPointerListener would
# never see pointers.  Treat kTexture like a proxy for hit testing.
if "WidgetType.kTexture)" not in text:
    old = (
        "    if (concreteWidgetType == WidgetType.kHero) {\n"
        "      return hitTestHero(result, position);\n"
        "    }\n"
    )
    new = (
        "    if (concreteWidgetType == WidgetType.kTexture) {\n"
        "      // Native views: report the hit without adding the listener here;\n"
        "      // hitTestWithPaintTransform adds it exactly once.\n"
        "      final RenderBox? textureChild = child;\n"
        "      return textureChild != null &&\n"
        "          textureChild.hasSize &&\n"
        "          textureChild.size.contains(position);\n"
        "    }\n"
        "\n"
        "    if (concreteWidgetType == WidgetType.kHero) {\n"
        "      return hitTestHero(result, position);\n"
        "    }\n"
    )
    if old not in text:
        raise SystemExit(f"cannot find hitTestChildren anchor in {path}")
    text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")
    print(f"patched native-view hit test in {path}")
else:
    # Upgrade checkouts patched with the first (double-add) variant.
    old_ht = (
        "    if (concreteWidgetType == WidgetType.kTexture) {\n"
        "      // Native views: let the texture widget's RenderPointerListener\n"
        "      // receive events (Linux headless placeholder path).\n"
        "      final RenderBox? textureChild = child;\n"
        "      return textureChild != null &&\n"
        "          textureChild.hitTest(result, position: position);\n"
        "    }\n"
    )
    new_ht = (
        "    if (concreteWidgetType == WidgetType.kTexture) {\n"
        "      // Native views: report the hit without adding the listener here;\n"
        "      // hitTestWithPaintTransform adds it exactly once.\n"
        "      final RenderBox? textureChild = child;\n"
        "      return textureChild != null &&\n"
        "          textureChild.hasSize &&\n"
        "          textureChild.size.contains(position);\n"
        "    }\n"
    )
    if old_ht in text:
        text = text.replace(old_ht, new_ht, 1)
        path.write_text(text, encoding="utf-8")
        print(f"upgraded native-view hit test in {path}")
    else:
        print(f"native-view hit test already patched in {path}")

# Connector-backed YGNodes: the shadow-node hit test above is authoritative.
# Without this guard the raw RenderBox fallback (YGConnectorRenderObject.hitTest
# == size.contains) makes every full-viewport fixed/portal overlay swallow all
# pointers, so scroll views behind them never receive gestures (the address-book
# root-portal A-Z index blocked the whole page).
if "concreteWidgetType != WidgetType.kYGConnector" not in text:
    old = (
        "    if (hitTestYGNode(result, position)) {\n"
        "      result.add(BoxHitTestEntry(this, position));\n"
        "      return true;\n"
        "    }\n"
    )
    new = (
        "    // Connector-backed nodes already ran the authoritative shadow-node hit\n"
        "    // test above.  YGConnectorRenderObject.hitTest only checks the whole box,\n"
        "    // so falling back to it would make a full-viewport fixed/portal overlay\n"
        "    // swallow pointers even when no child contains the point (blocking the\n"
        "    // scroll view behind the address-book A-Z index).\n"
        "    if (concreteWidgetType != WidgetType.kYGConnector &&\n"
        "        hitTestYGNode(result, position)) {\n"
        "      result.add(BoxHitTestEntry(this, position));\n"
        "      return true;\n"
        "    }\n"
    )
    if old not in text:
        raise SystemExit(f"cannot find hitTestWithPaintTransform anchor in {path}")
    text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")
    print(f"patched connector hit test in {path}")
else:
    print(f"connector hit test already patched in {path}")

# Headless Linux native-view fallback.  The public engine has no platform-view
# registry, so `SkylineTextureView` uses a synthetic texture id and a
# pointer-listening placeholder.  Pointer events still flow through
# `SkylineWidgetsFlutterBinding.forwardNativeViewPointerEvent` into the
# runtime's `HandleNativeViewPointerEvent`.
texture = app / "packages" / "skyline" / "lib" / "widgets" / "texture.dart"
texture_text = texture.read_text(encoding="utf-8")
if "Headless Linux" in texture_text:
    print(f"already patched {texture}")
else:
    old = (
        "    } else {\n"
        "      _controller = PlatformViewsService.initAndroidView(\n"
    )
    new = (
        "    } else if (Platform.isLinux) {\n"
        "      // Headless Linux: no platform-view registry.  Use a synthetic\n"
        "      // texture id and the pointer-listening placeholder below.\n"
        "      _textureId = id;\n"
        "    } else {\n"
        "      _controller = PlatformViewsService.initAndroidView(\n"
    )
    if old not in texture_text:
        raise SystemExit(f"cannot find initialize anchor in {texture}")
    texture_text = texture_text.replace(old, new, 1)

    old = (
        "    } else {\n"
        "      return createScaleRender(TransparentRenderAndroidView(\n"
    )
    new = (
        "    } else if (Platform.isLinux) {\n"
        "      // Headless Linux native-view placeholder.  Keep the pointer\n"
        "      // listener so events reach HandleNativeViewPointerEvent.\n"
        "      _decoratedBox = createScaleRender(RenderConstrainedBox(\n"
        "          additionalConstraints: const BoxConstraints.expand()));\n"
        "      return RenderPointerListener(\n"
        "        onPointerMove: sendPointerEvent,\n"
        "        onPointerCancel: sendPointerEvent,\n"
        "        onPointerDown: sendPointerEvent,\n"
        "        onPointerUp: sendPointerEvent,\n"
        "        child: _decoratedBox,\n"
        "      );\n"
        "    } else {\n"
        "      return createScaleRender(TransparentRenderAndroidView(\n"
    )
    if old not in texture_text:
        raise SystemExit(f"cannot find createRenderObject anchor in {texture}")
    texture_text = texture_text.replace(old, new, 1)

    old = (
        "    } else {\n"
        "      RenderAndroidView render =\n"
        "          updateScaleRender(renderObject) as RenderAndroidView;\n"
    )
    new = (
        "    } else if (Platform.isLinux) {\n"
        "      RenderPointerListener listener = renderObject as RenderPointerListener;\n"
        "      _decoratedBox = listener.child as RenderDecoratedBox;\n"
        "      updateScaleRender(_decoratedBox!);\n"
        "    } else {\n"
        "      RenderAndroidView render =\n"
        "          updateScaleRender(renderObject) as RenderAndroidView;\n"
    )
    if old not in texture_text:
        raise SystemExit(f"cannot find updateRenderObject anchor in {texture}")
    texture_text = texture_text.replace(old, new, 1)

    old = "    // Android: scale模式下任一方向有缩放即用黑色背景填充留白\n"
    new = (
        "    if (Platform.isLinux) {\n"
        "      // Opaque placeholder slot; native content lives outside the\n"
        "      // headless replay runtime.\n"
        "      return const BoxDecoration(color: Colors.black);\n"
        "    }\n"
        "\n"
        "    // Android: scale模式下任一方向有缩放即用黑色背景填充留白\n"
    )
    if old not in texture_text:
        raise SystemExit(f"cannot find getBoxDecoration anchor in {texture}")
    texture_text = texture_text.replace(old, new, 1)

    texture.write_text(texture_text, encoding="utf-8")
    print(f"patched {texture}")

# The placeholder's child does not hit-test itself, so make the listener
# opaque.  Kept separate so checkouts patched before this edit upgrade cleanly.
texture_text = texture.read_text(encoding="utf-8")
if "behavior: HitTestBehavior.opaque" not in texture_text:
    old = (
        "      return RenderPointerListener(\n"
        "        onPointerMove: sendPointerEvent,\n"
    )
    new = (
        "      return RenderPointerListener(\n"
        "        behavior: HitTestBehavior.opaque,\n"
        "        onPointerMove: sendPointerEvent,\n"
    )
    if old not in texture_text:
        raise SystemExit(f"cannot find pointer listener anchor in {texture}")
    texture_text = texture_text.replace(old, new, 1)
    texture.write_text(texture_text, encoding="utf-8")
    print(f"patched native-view hit test behavior in {texture}")
else:
    print(f"native-view hit test behavior already patched in {texture}")

# Optional HTTP platform-channel smoke hook: when SKYLINE_HTTP_TEST_URL is set,
# the devtools entrypoint performs one HttpForwarder.httpGet and prints the
# result.  This exercises ForwardHttpGet -> RequestHttpResource ->
# OnForwardHttpGetResponse end to end (used by the appservice ops test).
sample = app / "packages" / "sample" / "lib" / "devtools.dart"
sample_text = sample.read_text(encoding="utf-8")
if "SKYLINE_HTTP_TEST_URL" not in sample_text:
    imports = "import 'dart:io';\nimport 'package:skyline/utils/http_forwarder.dart';\n"
    sample_text = sample_text.replace(
        'import "package:skyline/skyline.dart";\n',
        'import "package:skyline/skyline.dart";\n' + imports,
        1,
    )
    hook = """
  final httpTestUrl = Platform.environment['SKYLINE_HTTP_TEST_URL'];
  if (httpTestUrl != null && httpTestUrl.isNotEmpty) {
    HttpForwarder.httpGet(httpTestUrl).then((data) {
      final text = data == null ? '' : String.fromCharCodes(data);
      print('SKYLINE_HTTP_RESULT ${data?.length ?? 0} $text');
    });
  }
"""
    marker = "      skylineAddonPath: args[5],\n  );\n"
    if marker not in sample_text:
        raise SystemExit(f"cannot find initSkylineForDevtools anchor in {sample}")
    sample_text = sample_text.replace(marker, marker + hook, 1)
    sample.write_text(sample_text, encoding="utf-8")
    print(f"patched {sample}")
else:
    print(f"already patched {sample}")

# Optional keyboard platform channel: the headless host cannot call into the
# Dart isolate from its own thread, so it sends a 'skyline/keyboard' platform
# message.  This handler forwards the payload to the embedded runtime export on
# the UI thread (used by the appservice ops keyboard test).
if "skyline/keyboard" not in sample_text:
    imports = (
        "import 'dart:convert';\n"
        "import 'dart:ffi';\n"
        "import 'package:ffi/ffi.dart';\n"
        "import 'package:flutter/services.dart';\n"
    )
    sample_text = sample_text.replace(
        'import "package:skyline/skyline.dart";\n',
        'import "package:skyline/skyline.dart";\n' + imports,
        1,
    )
    hook = """
  ServicesBinding.instance.defaultBinaryMessenger
      .setMessageHandler('skyline/keyboard', (ByteData? data) async {
    if (data == null) return null;
    final raw = utf8.decode(
        data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes));
    final parts = raw.split(' ');
    final kind = int.tryParse(parts.isNotEmpty ? parts[0] : '0') ?? 0;
    final logical =
        int.tryParse(parts.length > 1 ? parts[1] : '0') ?? 0;
    final text = parts.length > 2 ? parts.sublist(2).join(' ') : '';
    final library = DynamicLibrary.open('libskyline.so');
    final dispatch = library.lookupFunction<
        Void Function(Int32, Uint32, Pointer<Utf8>),
        void Function(int, int, Pointer<Utf8>)>('DispatchSkylineKeyEvent');
    final pointer = text.isEmpty ? nullptr : text.toNativeUtf8();
    dispatch(kind, logical, pointer);
    if (pointer != nullptr) calloc.free(pointer);
    return null;
  });
"""
    marker = "      skylineAddonPath: args[5],\n  );\n"
    if marker not in sample_text:
        raise SystemExit(f"cannot find initSkylineForDevtools anchor in {sample}")
    sample_text = sample_text.replace(marker, marker + hook, 1)
    sample.write_text(sample_text, encoding="utf-8")
    print(f"patched keyboard channel in {sample}")
else:
    print(f"keyboard channel already patched in {sample}")

# Custom routes mutate the host connector's children in
# `SkylineCupertinoPageTransition.build` (`children.clear()` + `children.add()`).
# The embedder passes widget children as a fixed-length Dart list, so every
# custom route build threw "Cannot change the length of a fixed-length list"
# and the pushed page stayed invisible.  Keep a growable copy in the base
# multi-child widget.
multi_path = (
    app / "packages" / "skyline" / "lib" / "widgets"
    / "multi_child_render_object_widget.dart"
)
multi_text = multi_path.read_text(encoding="utf-8")
old_ctor = (
    "  SkylineMultiChildRenderObjectWidget(\n"
    "      {Key? key, this.children = const <Widget>[]})\n"
    "      : assert(children != null),\n"
    "        super(key: key) {"
)
new_ctor = (
    "  SkylineMultiChildRenderObjectWidget(\n"
    "      {Key? key, List<Widget> children = const <Widget>[]})\n"
    "      : assert(children != null),\n"
    "        // Linux compat: the embedder passes a fixed-length list; custom\n"
    "        // routes mutate connector.children, so keep a growable copy.\n"
    "        children = List<Widget>.of(children),\n"
    "        super(key: key) {"
)
if old_ctor in multi_text:
    multi_path.write_text(
        multi_text.replace(old_ctor, new_ctor, 1), encoding="utf-8")
    print(f"patched growable children in {multi_path}")
elif new_ctor in multi_text:
    print(f"growable children already patched in {multi_path}")
else:
    raise SystemExit(
        "cannot find SkylineMultiChildRenderObjectWidget constructor in "
        f"{multi_path}")

# Flutter paints the "DEBUG" banner only when asserts are enabled (the JIT
# host kernel).  It needs a fallback font — before fontconfig was enabled the
# engine had none, so deterministic host fixtures were recorded without it.
# The DevTools runtime is release (no banner), so disable it in the simulated
# app to keep golden frames font-independent.
application = (
    app / "packages" / "skyline" / "lib" / "core" / "application.dart"
)
application_text = application.read_text(encoding="utf-8")
banner_old = "  bool showDebugBanner = true;"
banner_new = "  bool showDebugBanner = false;"
if banner_old in application_text:
    application.write_text(
        application_text.replace(banner_old, banner_new, 1), encoding="utf-8")
    print(f"disabled debug banner in {application}")
elif banner_new in application_text:
    print(f"debug banner already disabled in {application}")
else:
    raise SystemExit(f"cannot find showDebugBanner anchor in {application}")

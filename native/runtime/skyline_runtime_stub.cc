// Minimal Linux stand-in for the private `skyline.node` runtime.
//
// `package:skyline` opens `libskyline.so` (or the path passed as
// `skylineAddonPath`) and resolves the 73 symbols below.  This library exports
// the same ABI so the recompiled appservice kernel can get through
// `initSkylineForDevtools` and render its Flutter UI.  The embedded build
// materializes appservice operation streams through the real package:skyline
// callbacks (layout, text, scroll/list/grid, native image drawing) and bridges
// the HTTP/route/input/log platform channels.  Remaining gaps are listed in
// docs/appservice-ops.md section 6.
//
// ABI notes:
//   * all functions are `extern "C"`;
//   * argument lists are variadic where the payload is not needed, which is ABI
//     compatible on the SysV x86-64 ABI (the caller cleans the stack);
//   * return types MUST match the Dart `lookupFunction` native signature,
//     because the value is read from a specific register / struct-return slot.
//
// Keep the export list in sync with `spec/kernel-symbols.json`; the
// `skyline-runtime` test checks it.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef SKYLINE_EMBEDDED
// Statically linked into libflutter_engine.so: the full Dart API is available.
#include "third_party/dart/runtime/include/dart_api.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <fstream>
#include <iterator>
#include <string>
// Native image drawing: the direct-render paint callback receives a `dart:ui`
// Canvas.  The engine's C++ Canvas/DisplayList classes let us decode with Skia
// and append a DrawImageRect without round-tripping through Dart objects.
#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/image/dl_image.h"
#include "flutter/lib/ui/painting/canvas.h"
#include "flutter/lib/ui/painting/display_list_image_gpu.h"
#include "flutter/lib/ui/painting/image.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_wrappable.h"
// Schedule page construction after the current FFI callback returns; the Dart VM
// prohibits re-entering a native callback synchronously ("Cannot invoke native
// callback when API callbacks are prohibited").
#include "flutter/fml/message_loop.h"
#include "flutter/fml/task_runner.h"
#else
#if __has_include(<dart_api_dl.h>)
#include <dart_api_dl.h>
#endif
#endif

#if defined(_WIN32)
#define SKYLINE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define SKYLINE_EXPORT __attribute__((visibility("default")))
#else
#define SKYLINE_EXPORT
#endif

namespace {

struct FfiEntry {
  uint32_t hash;
  void* function;
};

// `Register` receives `stringHash(name)` values, not dense indices, so keep an
// append-only table.  The official runtime uses a hash lookup; the stub only
// needs to accept registrations for now.
struct FfiTable {
  uint32_t count = 0;
  FfiEntry entries[4096];
};

struct RuntimeHolder {
  FfiTable* ffi_table = nullptr;
  void* dart_isolate = nullptr;
};

struct Runtime {
  RuntimeHolder* holder = nullptr;
};

struct Window {
  Runtime* runtime = nullptr;
  int32_t id = 0;
};

// Holders created by CreateRuntimeHolder.  The DevTools entrypoint also passes
// an opaque holder pointer supplied by the host; when it was not created here
// (the stub host passes a non-zero sentinel), fall back to a shared holder.
std::vector<RuntimeHolder*> g_known_holders;
RuntimeHolder g_fallback_holder;

RuntimeHolder* HolderOrFallback(void* pointer) {
  auto* holder = static_cast<RuntimeHolder*>(pointer);
  for (RuntimeHolder* known : g_known_holders) {
    if (known == holder) {
      return holder;
    }
  }
  return &g_fallback_holder;
}

struct SkylineLayoutSize {
  double width;
  double height;
};

struct SkylineDryLayoutResult {
  float width;
  float height;
};

struct SkylineNativeRect {
  double left;
  double top;
  double right;
  double bottom;
  double width;
  double height;
};

bool g_dart_api_initialized = false;

// Viewport size reported by `CreateWindow`; used by the appservice replay to
// lay out the root page.
double g_viewport_width = 0.0;
double g_viewport_height = 0.0;

#ifdef SKYLINE_EMBEDDED
Dart_Isolate g_isolate = nullptr;
#endif

#ifdef SKYLINE_EMBEDDED
using SkylineHandle = Dart_Handle;

void LogTwo(const char* level, SkylineHandle tag, SkylineHandle message) {
  std::fprintf(stderr, "[skyline-runtime:%s] ", level);
  if (tag != nullptr && Dart_IsString(tag)) {
    const char* text = nullptr;
    if (!Dart_IsError(Dart_StringToCString(tag, &text)) && text != nullptr) {
      std::fprintf(stderr, "%s: ", text);
    }
  }
  if (message != nullptr && Dart_IsString(message)) {
    const char* text = nullptr;
    if (!Dart_IsError(Dart_StringToCString(message, &text)) && text != nullptr) {
      std::fprintf(stderr, "%s\n", text);
      return;
    }
  }
  std::fprintf(stderr, "<message>\n");
}
#else
using SkylineHandle = void*;

// The Dart API DL table does not expose string conversion, so log lines only
// carry the level.  Message text is still visible through
// `--entrypoint-arg` diagnostics / the Flutter host's own log callback.
void LogTwo(const char* level, SkylineHandle /*tag*/, SkylineHandle /*message*/) {
  std::fprintf(stderr, "[skyline-runtime:%s] <message>\n", level);
}
#endif

template <typename T>
intptr_t AsHandle(T* value) {
  return reinterpret_cast<intptr_t>(value);
}

}  // namespace

// ---------------------------------------------------------------------------
// Bootstrap / runtime objects
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" intptr_t InitDartAPIDLSkyline(void* data) {
#ifdef SKYLINE_EMBEDDED
  // The full Dart API is linked in; nothing to initialize.
  (void)data;
  g_dart_api_initialized = true;
  std::fprintf(stderr, "[skyline-runtime] embedded Dart API ready\n");
  return 0;
#else
#if __has_include(<dart_api_dl.h>)
  const intptr_t result = Dart_InitializeApiDL(data);
  g_dart_api_initialized = (result == 0);
  std::fprintf(stderr, "[skyline-runtime] Dart_InitializeApiDL -> %lld\n",
               static_cast<long long>(result));
  return result;
#else
  (void)data;
  return 0;
#endif
#endif
}

SKYLINE_EXPORT extern "C" intptr_t AllocateFfiTable() {
  auto* table = new FfiTable();
  return AsHandle(table);
}

SKYLINE_EXPORT extern "C" void Register(void* table_ptr, int32_t hash, void* function) {
  auto* table = static_cast<FfiTable*>(table_ptr);
  if (table == nullptr || table->count >= 4096) {
    return;
  }
  table->entries[table->count++] = FfiEntry{static_cast<uint32_t>(hash), function};
}

SKYLINE_EXPORT extern "C" intptr_t RuntimeHolderSetFfiTable(void* holder_ptr, void* table_ptr) {
  HolderOrFallback(holder_ptr)->ffi_table = static_cast<FfiTable*>(table_ptr);
  return 0;
}

SKYLINE_EXPORT extern "C" intptr_t CreateRuntimeHolder() {
  auto* holder = new RuntimeHolder();
  g_known_holders.push_back(holder);
  return AsHandle(holder);
}

SKYLINE_EXPORT extern "C" intptr_t ShareRuntimeFromRuntimeShell(void* holder_ptr, void* /*holder_object*/) {
  auto* runtime = new Runtime();
  runtime->holder = HolderOrFallback(holder_ptr);
  return AsHandle(runtime);
}

SKYLINE_EXPORT extern "C" intptr_t CreateWindow(
    void* /*runtime_ptr*/, int32_t /*id*/, void* /*ffi_table_ptr*/,
    double window_width, double window_height, double /*padding_top*/,
    double /*padding_right*/, double /*padding_bottom*/,
    double /*padding_left*/, double /*device_pixel_ratio*/,
    void* /*window_holder*/) {
  g_viewport_width = window_width;
  g_viewport_height = window_height;
  return AsHandle(new Window());
}

SKYLINE_EXPORT extern "C" intptr_t CreateEmptyWindow(
    void* /*runtime_ptr*/, int32_t /*id*/, void* /*ffi_table_ptr*/,
    void* /*window_holder*/) {
  return AsHandle(new Window());
}

SKYLINE_EXPORT extern "C" void SetCurrentDartIsolate(void* holder_ptr) {
  RuntimeHolder* holder = HolderOrFallback(holder_ptr);
  holder->dart_isolate = holder;
#ifdef SKYLINE_EMBEDDED
  g_isolate = Dart_CurrentIsolate();
#endif
}

namespace {

void* FindCallback(FfiTable* table, uint32_t hash) {
  if (table == nullptr) {
    return nullptr;
  }
  for (uint32_t i = 0; i < table->count; ++i) {
    if (table->entries[i].hash == hash) {
      return table->entries[i].function;
    }
  }
  return nullptr;
}

// stringHash('RunApplication') from spec/kernel-symbols.json.
constexpr uint32_t kRunApplicationHash = 548712037u;

}  // namespace

#ifdef SKYLINE_EMBEDDED
void CreateDemoPage(RuntimeHolder* holder);
void CreateTextPage(RuntimeHolder* holder);
void CreateJsonPage(RuntimeHolder* holder, const char* descriptor);
void CreateAppServiceOpsPage(RuntimeHolder* holder, const char* descriptor);
void ScheduleCreateDemoPage(RuntimeHolder* holder);
#endif

// The official runtime schedules `RunApplication` on the UI task runner.  The
// stub calls the callback synchronously; NotifyFlutterReady is invoked from
// Dart, so the FFI callback trampoline is already on the isolate thread.
SKYLINE_EXPORT extern "C" void NotifyFlutterReady(void* holder_ptr) {
  RuntimeHolder* holder = HolderOrFallback(holder_ptr);
  void* callback = FindCallback(holder->ffi_table, kRunApplicationHash);
  if (callback != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] NotifyFlutterReady -> RunApplication\n");
    reinterpret_cast<void (*)()>(callback)();
#ifdef SKYLINE_EMBEDDED
    ScheduleCreateDemoPage(holder);
#endif
  } else {
    std::fprintf(stderr, "[skyline-runtime] NotifyFlutterReady: RunApplication not registered\n");
  }
}

#ifdef SKYLINE_EMBEDDED

namespace {
// Defined with the resource-loading platform channel further down; needed here
// by the native image loader.
std::string ResolveResourcePath(const std::string& path);
bool ReadResourceBytes(const std::string& path, std::vector<uint8_t>* out);
}  // namespace

// ---------------------------------------------------------------------------
// Synthetic Skyline page (embedded build)
//
// With the full Dart API available we can call the real `package:skyline`
// constructors.  This builds a minimal tree:
//
//   CreatePageRoute
//     +- YGNode (root, yoga layout buffer)
//          +- DecoratedBox (solid #FF3366)
//
// The yoga layout callback fills the root's 9-float metrics buffer.
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<intptr_t, float*> g_yoga_metrics;
bool g_demo_page_created = false;

struct SkylinePageConfig {
  uint32_t backgroundColorContent;
  void* pageUrl;
};

using NewDecoratedBoxFn = Dart_Handle (*)(
    intptr_t ptr,
    uint32_t backgroundColor,
    double borderLeftWidth,
    double borderTopWidth,
    double borderRightWidth,
    double borderBottomWidth,
    uint32_t borderLeftColor,
    uint32_t borderTopColor,
    uint32_t borderRightColor,
    uint32_t borderBottomColor,
    uint8_t borderLeftStyle,
    uint8_t borderTopStyle,
    uint8_t borderRightStyle,
    uint8_t borderBottomStyle,
    double borderTLRadiusX,
    double borderTLRadiusY,
    double borderTRRadiusX,
    double borderTRRadiusY,
    double borderBLRadiusX,
    double borderBLRadiusY,
    double borderBRRadiusX,
    double borderBRRadiusY,
    uint8_t borderTLRadiusXPercent,
    uint8_t borderTLRadiusYPercent,
    uint8_t borderTRRadiusXPercent,
    uint8_t borderTRRadiusYPercent,
    uint8_t borderBLRadiusXPercent,
    uint8_t borderBLRadiusYPercent,
    uint8_t borderBRRadiusXPercent,
    uint8_t borderBRRadiusYPercent,
    Dart_Handle imageUrl,
    uint8_t backgroundSideX,
    uint8_t backgroundSideY,
    double alignmentX,
    double alignmentY,
    double backgroundOffsetX,
    double backgroundOffsetY,
    uint8_t repeat,
    uint8_t fit,
    double fitWidth,
    double fitHeight,
    uint8_t fitWidthPercent,
    uint8_t fitHeightPercent,
    Dart_Handle gradient,
    uint8_t backgroundImageLayerMode,
    Dart_Handle boxShadows,
    uint8_t hasMaskImage,
    void* onPaint,
    void* onCreateRenderObject,
    void* onDisposed,
    void* onPaintMask,
    Dart_Handle child);

using NewYGNodeFn = Dart_Handle (*)(
    intptr_t yogaNodeAddress,
    Dart_Handle yogaLayout,
    uint8_t isRoot,
    uint8_t isRepaintBoundary,
    uint8_t enableLayoutParagraph,
    uint8_t constraintsType,
    uint32_t concreteWidgetType,
    Dart_Handle transformSharedData,
    double originX,
    double originY,
    double alignmentX,
    double alignmentY,
    uint8_t visible,
    uint8_t catchEvent,
    uint8_t pointerEvents,
    Dart_Handle intersections,
    Dart_Handle yogaConnector,
    Dart_Handle child);

using NewYGConnectorFn = Dart_Handle (*)(uint8_t enableStackingPaint,
                                          Dart_Handle children);

using NewTextSpanFn = Dart_Handle (*)(
    intptr_t textSpanAddress,
    Dart_Handle text,
    uint32_t color,
    uint32_t backgroundColor,
    uint8_t textDecoration,
    uint8_t textDecorationStyle,
    uint32_t decorationColor,
    double decorationThickness,
    void* fontFeatures,
    uint32_t fontFeaturesLength,
    double fontSize,
    uint8_t fontWeight,
    uint8_t fontStyle,
    Dart_Handle fontFamilyFallback,
    double height,
    double letterSpacing,
    double wordSpacing,
    uint8_t wordBreak,
    Dart_Handle shadows,
    Dart_Handle children);

using NewTextFn = Dart_Handle (*)(
    intptr_t ptr,
    void* markDirty,
    void* createParagraph,
    uint8_t textAlign,
    uint8_t textDirection,
    uint8_t overflow,
    uint8_t softWrap,
    uint32_t maxLines,
    uint8_t userSelect,
    uint8_t selectOnGesture,
    uint8_t enableNativeParagraph,
    Dart_Handle children);

using NewOpacityFn = Dart_Handle (*)(double opacity, Dart_Handle child);

using NewLinearGradientFn = Dart_Handle (*)(
    double radianAngle,
    Dart_Handle colors,
    void* stops,
    double fitWidth,
    double fitHeight,
    uint8_t fitWidthPercent,
    uint8_t fitHeightPercent,
    uint8_t backgroundSideX,
    uint8_t backgroundSideY,
    double alignmentX,
    double alignmentY,
    double backgroundOffsetX,
    double backgroundOffsetY,
    uint8_t repeat);

struct GradientStopNative {
  double value;
  uint8_t isPercent;
};

using NewBoxShadowFn = Dart_Handle (*)(uint32_t shadowColor,
                                       double shadowOffsetX,
                                       double shadowOffsetY,
                                       double blurRadius,
                                       double spreadRadius,
                                       uint8_t shadowInset);

using DecoratedBoxUpdateColorFn = void (*)(Dart_Handle decoratedBox,
                                           uint32_t backgroundColor);
using TextSpanUpdateTextFn = void (*)(Dart_Handle text, Dart_Handle data);
using TextSpanUpdateTextColorFn = void (*)(Dart_Handle text, uint32_t color);
using TextSpanUpdateFontSizeFn = void (*)(Dart_Handle text, double size);
using TextSpanUpdateFontWeightFn = void (*)(Dart_Handle text, uint8_t weight);
using TextSpanUpdateFontStyleFn = void (*)(Dart_Handle text, uint8_t style);
using TextSpanUpdateLineHeightFn = void (*)(Dart_Handle text, double height);
using TextSpanUpdateLetterSpacingFn = void (*)(Dart_Handle text, double spacing);
using TextApplyUpdateFn = void (*)(Dart_Handle text, uint8_t subtreeChanged);
using TextUpdateTextAlignFn = void (*)(Dart_Handle text, uint8_t align);
using TextUpdateMaxLinesFn = void (*)(Dart_Handle text, uint32_t maxLines);
using LoadFontFaceFn = void (*)(Dart_Handle family, Dart_Handle urls,
                                int32_t timeout_ms, int32_t callback_id);
using NotifyResourceLoadFn = void (*)(uint8_t callback_id, Dart_Handle pixels);

using NewScrollViewFn = Dart_Handle (*)(
    intptr_t ptr, uint32_t view_tag, uint8_t type, uint8_t scroll_x,
    uint8_t scroll_y, double scroll_top, double scroll_left,
    uint8_t scroll_with_animation, double lower_threshold,
    double upper_threshold, uint8_t refresher_enabled,
    double refresher_threshold, uint32_t refresher_background,
    uint8_t refresher_default_style, uint8_t refresher_triggered,
    uint8_t pinned_when_open_two_level, uint8_t enable_scroll_when_two_level,
    uint8_t enable_ballistic_refresh, uint8_t two_level_enabled,
    uint8_t two_level_triggered, double two_level_threshold,
    double close_two_level_threshold, uint8_t scroll_enabled, uint8_t bounces,
    uint8_t show_scrollbar, uint8_t fast_deceleration, uint8_t reverse,
    double cache_extent, void* recognizers, uint32_t recognizer_num,
    Dart_Handle refresher_header, uint8_t disable_clip,
    double minimum_drag_distance, double padding_top, double padding_right,
    double padding_bottom, double padding_left, uint8_t associative_container,
    uint8_t paint_order, uint8_t scroll_anchoring,
    uint8_t inner_correction_mode, void* compute_content_size,
    void* on_scroll_start, void* on_scroll, void* on_scroll_end,
    void* on_scroll_to_lower, void* on_scroll_to_upper,
    void* on_refresher_pulling, void* on_refresher_will_refresh,
    void* on_refresher_refresh, void* on_refresher_restore,
    void* on_refresher_abort, void* on_refresher_status_change,
    void* on_mark_needs_layout, Dart_Handle children);

// `NewListView`/`NewGridView` build the virtualized slivers used by
// `<list-view>`/`<grid-view>`.  The per-item mount callbacks are optional
// (`Pointer` may be null); the replay supplies every item up front.
using ListViewItemCallbackFn = void (*)(intptr_t ptr, int32_t index);
using NewListViewFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                      double padding_top, double padding_right,
                                      double padding_bottom,
                                      double padding_left,
                                      uint32_t background_color,
                                      void* on_mount_child,
                                      void* on_unmount_child);
using NewGridViewFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                      uint8_t type, int32_t cross_axis_count,
                                      float main_axis_spacing,
                                      float cross_axis_spacing,
                                      float max_cross_axis_extent,
                                      double padding_top, double padding_right,
                                      double padding_bottom,
                                      double padding_left,
                                      uint32_t background_color);

// `NewDirectRenderWidget` installs a leaf widget whose paint callback receives
// the Dart `Canvas`; images decode + draw natively (see DirectRenderImagePaint).
using NewDirectRenderWidgetFn = Dart_Handle (*)(intptr_t ptr, void* on_paint,
                                                void* on_dispose,
                                                void* on_create_render_object,
                                                void* on_layout);

// List/grid builders.  `on_build_child`/`on_dispose_child` are native
// function pointers invoked by the Dart sliver (`(ptr, index)`).
using BuilderChildCallbackFn = void (*)(intptr_t ptr, int32_t index);
using NewListBuilderFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                         uint8_t type, int32_t child_count,
                                         float child_height,
                                         double padding_top,
                                         double padding_right,
                                         double padding_bottom,
                                         double padding_left,
                                         uint32_t background_color,
                                         void* on_build_child,
                                         void* on_dispose_child);
using NewGridBuilderFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                         uint8_t type, int32_t child_count,
                                         int32_t cross_axis_count,
                                         float main_axis_spacing,
                                         float cross_axis_spacing,
                                         float max_cross_axis_extent,
                                         double padding_top,
                                         double padding_right,
                                         double padding_bottom,
                                         double padding_left,
                                         uint32_t background_color,
                                         void* on_build_child,
                                         void* on_dispose_child);
using NewListBuilderItemFn = Dart_Handle (*)(Dart_Handle children,
                                              int32_t index);
using BuilderMutateChildFn = void (*)(Dart_Handle parent, Dart_Handle child);
using BuilderInsertBeforeFn = void (*)(Dart_Handle parent, Dart_Handle child,
                                       Dart_Handle pivot);
using BuilderUpdateChildrenFn = void (*)(Dart_Handle parent,
                                         Dart_Handle children);
using BuilderUpdateIntFn = void (*)(Dart_Handle builder, int32_t value);
using BuilderUpdateFloatFn = void (*)(Dart_Handle builder, double value);
using BuilderUpdatePaddingFn = void (*)(Dart_Handle builder, double top,
                                        double right, double bottom,
                                        double left);
using BuilderUpdateColorFn = void (*)(Dart_Handle builder, uint32_t color);
using NavigateBackFn = void (*)(int32_t delta, uint8_t animated,
                                int32_t callback_id);
using SwitchTabFn = void (*)(int32_t page_id, int32_t callback_id);
using UpdateKeyboardStatusFn = void (*)(int32_t input_key, double height);
using InputValueFn = void (*)(Dart_Handle input, Dart_Handle value);
using InputIntFn = void (*)(Dart_Handle input, int32_t value);
using InputBoolFn = void (*)(Dart_Handle input, uint8_t value);
using InputFocusFn = void (*)(Dart_Handle input, uint8_t focus);
using NewInputFn = Dart_Handle (*)(
    intptr_t ptr, void* markDirty, int32_t viewId, uint32_t viewTag,
    uint8_t multiLine, Dart_Handle value, Dart_Handle type, uint8_t password,
    Dart_Handle placeholder, double placeholderFontSize,
    uint8_t placeholderFontWeight, uint32_t placeholderFontColor,
    uint8_t disable, int32_t maxLength, int32_t cursorSpacing,
    uint32_t cursorColor, uint8_t focus, uint8_t autoHeight,
    Dart_Handle confirmType, uint8_t confirmHold, int32_t cursor,
    int32_t selectionStart, int32_t selectionEnd, uint8_t showConfirmBar,
    uint8_t adjustPosition, uint8_t holdKeyboard, uint8_t textAlign,
    uint32_t color, uint32_t backgroundColor, uint8_t textDecoration,
    uint8_t textDecorationStyle, uint32_t decorationColor,
    double decorationThickness, void* fontFeatures, uint32_t fontFeaturesLength,
    double fontSize, double lineHeight, uint8_t fontWeight, uint8_t fontStyle,
    Dart_Handle fontFamilyFallback, double letterSpacing, double wordSpacing,
    Dart_Handle shadows, void* nativeOnInput, void* nativeOnFocus,
    void* nativeOnBlur, void* nativeOnLineChange, void* nativeOnConfirm,
    void* nativeOnKeyboardHeightChange, void* nativeOnSelectionChange,
    void* nativeOnCompositionStart, void* nativeOnCompositionEnd,
    void* nativeOnCompositionUpdate, void* nativeOnSetValueCompleted,
    void* nativeOnPageBottomPaddingChange);
using YGNodeSetupTransformFn = void (*)(Dart_Handle yoga_node,
                                        Dart_Handle matrix);
using YGNodeTransformOriginFn = void (*)(Dart_Handle yoga_node, double origin_x,
                                         double origin_y, double align_x,
                                         double align_y);

using DispatchTouchEventFn = void (*)(uint8_t type, double x, double y);
using ScrollViewUpdateScrollTopFn = void (*)(Dart_Handle scroll_view, double top);
using ConnectorUpdateChildrenFn = void (*)(Dart_Handle connector,
                                           uint8_t stackingPaint,
                                           Dart_Handle children);
using MarkYGNodeNeedsLayoutFn = void (*)(Dart_Handle yogaNode);
using OpacityUpdateFn = void (*)(Dart_Handle widget, double opacity);
using DecoratedBoxUpdateBorderFn = void (*)(Dart_Handle decoratedBox,
                                            double leftWidth,
                                            double topWidth,
                                            double rightWidth,
                                            double bottomWidth,
                                            uint32_t leftColor,
                                            uint32_t topColor,
                                            uint32_t rightColor,
                                            uint32_t bottomColor,
                                            uint8_t leftStyle,
                                            uint8_t topStyle,
                                            uint8_t rightStyle,
                                            uint8_t bottomStyle);
using DecoratedBoxUpdateShadowFn = void (*)(Dart_Handle decoratedBox,
                                            Dart_Handle shadows);
using DecoratedBoxUpdateRadiusFn = void (*)(Dart_Handle decoratedBox,
                                            uint8_t side, double x, double y,
                                            uint8_t x_is_percent,
                                            uint8_t y_is_percent);
using DecoratedBoxUpdateGradientFn = void (*)(Dart_Handle decoratedBox,
                                              Dart_Handle gradient);
using DecoratedBoxClearGradientFn = void (*)(Dart_Handle decoratedBox);
using ClipRRectUpdateRadiusFn = void (*)(
    Dart_Handle clip_rrect, double top_left_x, double top_left_y,
    double top_right_x, double top_right_y, double bottom_left_x,
    double bottom_left_y, double bottom_right_x, double bottom_right_y,
    uint8_t top_left_x_pct, uint8_t top_left_y_pct, uint8_t top_right_x_pct,
    uint8_t top_right_y_pct, uint8_t bottom_left_x_pct,
    uint8_t bottom_left_y_pct, uint8_t bottom_right_x_pct,
    uint8_t bottom_right_y_pct);

using NewClipRRectFn = Dart_Handle (*)(
    uint8_t disableClip,
    double topLeftRadiusX,
    double topLeftRadiusY,
    double topRightRadiusX,
    double topRightRadiusY,
    double bottomLeftRadiusX,
    double bottomLeftRadiusY,
    double bottomRightRadiusX,
    double bottomRightRadiusY,
    uint8_t topLeftRadiusXPercent,
    uint8_t topLeftRadiusYPercent,
    uint8_t topRightRadiusXPercent,
    uint8_t topRightRadiusYPercent,
    uint8_t bottomLeftRadiusXPercent,
    uint8_t bottomLeftRadiusYPercent,
    uint8_t bottomRightRadiusXPercent,
    uint8_t bottomRightRadiusYPercent,
    double borderLeftWidth,
    double borderTopWidth,
    double borderRightWidth,
    double borderBottomWidth,
    Dart_Handle child);

using CreatePageRouteFn = void (*)(
    int32_t callbackId,
    uint8_t openType,
    uint8_t isCustomRoute,
    uint8_t isTabPage,
    int32_t pageId,
    uint8_t opaque,
    uint8_t maintainState,
    uint32_t transitionDuration,
    uint32_t reverseTransitionDuration,
    uint32_t barrierColor,
    uint8_t hasBarrierColor,
    uint8_t barrierDismissible,
    Dart_Handle barrierLabel,
    uint8_t canTransitionTo,
    uint8_t canTransitionFrom,
    uint8_t forceSecondaryAnimation,
    uint8_t popGestureEnabled,
    uint8_t allowEnterRouteSnapshotting,
    uint8_t allowExitRouteSnapshotting,
    uint8_t fullscreenDrag,
    uint8_t dismissDirection,
    Dart_Handle animationHash,
    Dart_Handle secondaryAnimationHash,
    Dart_Handle host,
    Dart_Handle root,
    void* config);

constexpr uint32_t kCreatePageRouteHash = 662439267u;
constexpr uint32_t kNewDecoratedBoxHash = 548213892u;
constexpr uint32_t kNewYGNodeHash = 644570152u;
constexpr uint32_t kNewYGConnectorHash = 517238689u;
constexpr uint32_t kNewTextHash = 738901768u;
constexpr uint32_t kNewTextSpanHash = 553157049u;
constexpr uint32_t kNewOpacityHash = 357244940u;
constexpr uint32_t kNewClipRRectHash = 744380804u;
constexpr uint32_t kNewLinearGradientHash = 753559496u;
constexpr uint32_t kNewBoxShadowHash = 459522077u;
constexpr uint32_t kDecoratedBoxUpdateColorHash = 533446964u;
constexpr uint32_t kYGConnectorUpdateChildrenHash = 542320778u;
constexpr uint32_t kMarkYGNodeNeedsLayoutHash = 739701891u;
constexpr uint32_t kOpacityUpdateHash = 362821014u;
constexpr uint32_t kDecoratedBoxUpdateBorderHash = 515044687u;
constexpr uint32_t kDecoratedBoxUpdateShadowHash = 458311868u;
constexpr uint32_t kDecoratedBoxUpdateBorderSideRadiusHash = 464860390u;
constexpr uint32_t kDecoratedBoxUpdateGradientHash = 756142529u;
constexpr uint32_t kDecoratedBoxClearGradientHash = 754568836u;
constexpr uint32_t kClipRRectUpdateRadiusHash = 450816005u;
constexpr uint32_t kTextSpanUpdateTextHash = 752549844u;
constexpr uint32_t kTextSpanUpdateTextColorHash = 504732227u;
constexpr uint32_t kTextSpanUpdateFontSizeHash = 666327014u;
constexpr uint32_t kTextSpanUpdateFontWeightHash = 745670781u;
constexpr uint32_t kTextSpanUpdateFontStyleHash = 640254097u;
constexpr uint32_t kTextSpanUpdateLineHeightHash = 756511120u;
constexpr uint32_t kTextSpanUpdateLetterSpacingHash = 527068474u;
constexpr uint32_t kTextApplyUpdateHash = 651704001u;
constexpr uint32_t kTextUpdateTextAlignHash = 549308516u;
constexpr uint32_t kTextUpdateMaxLinesHash = 464602096u;
constexpr uint32_t kLoadFontFaceHash = 643604203u;
constexpr uint32_t kNotifyResourceLoadHash = 729783546u;
constexpr uint32_t kDispatchTouchEventHash = 746946373u;
constexpr uint32_t kNewScrollViewHash = 457879666u;
constexpr uint32_t kNewListViewHash = 461881501u;
constexpr uint32_t kNewGridViewHash = 440905746u;
constexpr uint32_t kNewDirectRenderWidgetHash = 755744935u;
constexpr uint32_t kRequestHttpResourceHash = 670378368u;
constexpr uint32_t kOnForwardHttpGetResponseHash = 663443835u;
constexpr uint32_t kGetNodeFromPointHash = 771509372u;
constexpr uint32_t kScrollViewUpdateScrollTopHash = 390858736u;
constexpr uint32_t kNewListBuilderHash = 526654284u;
constexpr uint32_t kNewListBuilderItemHash = 629516111u;
constexpr uint32_t kListBuilderAppendChildHash = 707989954u;
constexpr uint32_t kListBuilderRemoveChildHash = 721255976u;
constexpr uint32_t kListBuilderInsertBeforeHash = 641555288u;
constexpr uint32_t kListBuilderUpdateChildrenHash = 544349630u;
constexpr uint32_t kListBuilderUpdateChildCountHash = 769400916u;
constexpr uint32_t kListBuilderUpdateChildHeightHash = 738571139u;
constexpr uint32_t kListBuilderUpdatePaddingHash = 527346916u;
constexpr uint32_t kListBuilderUpdateBackgroundColorHash = 528923708u;
constexpr uint32_t kListBuilderItemUpdateChildrenHash = 549538126u;
constexpr uint32_t kListBuilderItemUpdateIndexHash = 556218809u;
constexpr uint32_t kNewGridBuilderHash = 506712993u;
constexpr uint32_t kNewGridBuilderItemHash = 615943092u;
constexpr uint32_t kGridBuilderAppendChildHash = 720875816u;
constexpr uint32_t kGridBuilderRemoveChildHash = 719309313u;
constexpr uint32_t kGridBuilderInsertBeforeHash = 639062234u;
constexpr uint32_t kGridBuilderUpdateChildrenHash = 542877480u;
constexpr uint32_t kGridBuilderUpdateChildCountHash = 753287585u;
constexpr uint32_t kGridBuilderUpdateCrossAxisCountHash = 750670620u;
constexpr uint32_t kGridBuilderUpdateMainAxisSpacingHash = 534430105u;
constexpr uint32_t kGridBuilderUpdateCrossAxisSpacingHash = 530243228u;
constexpr uint32_t kGridBuilderUpdateMaxCrossAxisExtentHash = 759854312u;
constexpr uint32_t kGridBuilderUpdatePaddingHash = 519451945u;
constexpr uint32_t kGridBuilderUpdateBackgroundColorHash = 514461315u;
constexpr uint32_t kGridBuilderItemUpdateChildrenHash = 551358966u;
constexpr uint32_t kGridBuilderItemUpdateIndexHash = 562990716u;
constexpr uint32_t kNavigateBackHash = 802972930u;
constexpr uint32_t kSwitchTabHash = 958852003u;
constexpr uint32_t kUpdateKeyboardStatusHash = 467802059u;
constexpr uint32_t kInputValueHash = 660826392u;
constexpr uint32_t kInputCursorHash = 532468509u;
constexpr uint32_t kInputSelectionStartHash = 760722848u;
constexpr uint32_t kInputSelectionEndHash = 712315579u;
constexpr uint32_t kInputHoldKeyboardHash = 720865937u;
constexpr uint32_t kInputFocusHash = 453329760u;
constexpr uint32_t kNewInputHash = 762417174u;
constexpr uint32_t kYGNodeSetupTransformHash = 620430677u;
constexpr uint32_t kYGNodeTransformOriginHash = 740488219u;

enum class DemoMode { kStripes, kText, kCard };
DemoMode g_demo_mode = DemoMode::kStripes;

// No-op native callbacks passed to NewText.  They are only invoked when the
// text needs a dirty notification / native paragraph; neither affects the
// Flutter-rendered text path (enableNativeParagraph=false).
extern "C" void DemoMarkDirty(intptr_t /*ptr*/) {}
extern "C" void DemoCreateParagraph(intptr_t /*ptr*/, Dart_Handle /*paragraph*/,
                                    float /*maxWidth*/, uint8_t /*isDryLayout*/) {}

// Layout model for the synthetic page: a vertical column of stripes.
struct DemoNode {
  intptr_t address;
  int index;  // 0 = root, 1..n = child stripes (top to bottom)
};
std::vector<DemoNode> g_demo_nodes;

// Descriptor-driven page (`SKYLINE_STUB_PAGE={...}`): nested row/column flex.
struct JsonNode {
  intptr_t address = 0;
  bool row = false;
  float padding = 0.0f;
  float gap = 0.0f;
  float flex = 0.0f;
  float main_size = 0.0f;  // NaN when flexible
  std::vector<JsonNode> children;
};
JsonNode g_json_root;
bool g_json_active = false;

struct PendingColorUpdate {
  Dart_PersistentHandle decorated_box;
  uint32_t color;
  int delay_ms;
};
std::vector<PendingColorUpdate> g_pending_color_updates;

Dart_PersistentHandle g_json_root_connector = nullptr;
struct RootChildRecord {
  Dart_PersistentHandle handle;
  intptr_t address;
};
std::vector<RootChildRecord> g_json_root_child_records;

enum class StyleUpdateKind { kOpacity, kBorder };
struct PendingStyleUpdate {
  Dart_PersistentHandle handle;
  StyleUpdateKind kind;
  double value;
  double width;
  uint32_t color;
  int delay_ms;
};
std::vector<PendingStyleUpdate> g_pending_style_updates;
OpacityUpdateFn g_update_opacity = nullptr;
DecoratedBoxUpdateBorderFn g_update_border = nullptr;
intptr_t g_json_next_address = 0x1100;

void LayoutJsonNode(const JsonNode& node, float left, float top, float width,
                    float height);

void ReportDartError(const char* what, Dart_Handle handle) {
  const char* error = Dart_IsError(handle) ? Dart_GetError(handle) : "<not an error>";
  std::fprintf(stderr, "[skyline-runtime] demo page: %s failed: %s\n", what,
               error != nullptr ? error : "<no message>");
#ifdef SKYLINE_EMBEDDED
  if (!Dart_IsError(handle)) return;
  Dart_Handle exception = Dart_ErrorGetException(handle);
  if (!Dart_IsError(exception) && !Dart_IsNull(exception)) {
    Dart_Handle text = Dart_ToString(exception);
    if (!Dart_IsError(text)) {
      const char* message = nullptr;
      if (!Dart_IsError(Dart_StringToCString(text, &message)) &&
          message != nullptr) {
        std::fprintf(stderr, "[skyline-runtime] demo page: exception: %s\n",
                     message);
      }
    }
  }
  Dart_Handle stack = Dart_ErrorGetStackTrace(handle);
  if (!Dart_IsError(stack) && !Dart_IsNull(stack)) {
    Dart_Handle text = Dart_ToString(stack);
    if (!Dart_IsError(text)) {
      const char* message = nullptr;
      if (!Dart_IsError(Dart_StringToCString(text, &message)) &&
          message != nullptr) {
        std::fprintf(stderr, "[skyline-runtime] demo page: stack:\n%s\n",
                     message);
      }
    }
  }
#endif
}

// Enters the root isolate/scope for the posted UI task; released on every exit
// path (including constructor errors) so later tasks can re-enter safely.
class DemoPageApiScope {
 public:
  DemoPageApiScope() {
    if (Dart_CurrentIsolate() == nullptr && g_isolate != nullptr) {
      Dart_EnterIsolate(g_isolate);
      entered_isolate_ = true;
    }
    Dart_EnterScope();
  }

  ~DemoPageApiScope() {
    Dart_ExitScope();
    if (entered_isolate_) {
      Dart_ExitIsolate();
    }
  }

 private:
  bool entered_isolate_ = false;
};

fml::RefPtr<fml::TaskRunner> UiTaskRunner() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  auto runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
#pragma GCC diagnostic pop
  return runner;
}

void SchedulePendingStyleUpdates() {
  if (g_pending_style_updates.empty()) {
    return;
  }
  auto task_runner = UiTaskRunner();
  if (!task_runner) {
    g_pending_style_updates.clear();
    return;
  }
  for (const PendingStyleUpdate& update : g_pending_style_updates) {
    task_runner->PostDelayedTask(
        [update]() {
          if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
            return;
          }
          DemoPageApiScope scope;
          Dart_Handle handle = Dart_HandleFromPersistent(update.handle);
          if (update.kind == StyleUpdateKind::kOpacity) {
            if (g_update_opacity != nullptr) {
              g_update_opacity(handle, update.value);
            }
          } else if (g_update_border != nullptr) {
            const uint8_t style = update.width > 0 ? 1 : 0;
            g_update_border(handle, update.width, update.width, update.width,
                            update.width, update.color, update.color,
                            update.color, update.color, style, style, style,
                            style);
          }
          Dart_DeletePersistentHandle(update.handle);
        },
        fml::TimeDelta::FromMilliseconds(update.delay_ms));
  }
  g_pending_style_updates.clear();
}

}  // namespace

void CreateDemoPage(RuntimeHolder* holder) {
  const char* ops_descriptor = std::getenv("SKYLINE_APP_SERVICE_OPS");
  if (ops_descriptor == nullptr && std::getenv("SKYLINE_STUB_PAGE") == nullptr) {
    return;  // opt-in: synthetic page for tests, not the default appservice UI
  }
  if (g_demo_page_created || holder == nullptr) {
    return;
  }
  if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] demo page: no isolate available\n");
    return;
  }
  DemoPageApiScope scope;
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* createPageRoute = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  if (newDecoratedBox == nullptr || newYGNode == nullptr ||
      createPageRoute == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] demo page callbacks not registered "
                 "(decorated=%p yg=%p route=%p)\n",
                 reinterpret_cast<void*>(newDecoratedBox),
                 reinterpret_cast<void*>(newYGNode),
                 reinterpret_cast<void*>(createPageRoute));
    return;
  }
  g_demo_page_created = true;
  if (ops_descriptor != nullptr) {
    g_json_active = true;
    CreateAppServiceOpsPage(holder, ops_descriptor);
    return;
  }
  const char* descriptor = std::getenv("SKYLINE_STUB_PAGE_JSON");
  if (descriptor == nullptr) {
    const char* page = std::getenv("SKYLINE_STUB_PAGE");
    if (page != nullptr && (page[0] == '{' || page[0] == '@')) {
      descriptor = page;
    }
  }
  if (descriptor != nullptr) {
    g_json_active = true;
    CreateJsonPage(holder, descriptor);
    return;
  }
  const char* mode = std::getenv("SKYLINE_STUB_PAGE");
  if (mode != nullptr && std::strcmp(mode, "text") == 0) {
    g_demo_mode = DemoMode::kText;
    CreateTextPage(holder);
    return;
  }
  if (mode != nullptr && std::strcmp(mode, "card") == 0) {
    g_demo_mode = DemoMode::kCard;
    CreateTextPage(holder);
    return;
  }
  g_demo_mode = DemoMode::kStripes;

  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  if (newYGConnector == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] demo page: NewYGConnector not registered\n");
    return;
  }

  Dart_Handle empty = Dart_NewStringFromCString("");
  constexpr int kChildCount = 3;
  const uint32_t colors[kChildCount] = {0xFFFF3366u, 0xFF33CC66u, 0xFF3366FFu};
  g_demo_nodes.clear();

  Dart_Handle children = Dart_NewList(kChildCount);
  if (Dart_IsError(children)) {
    ReportDartError("Dart_NewList", children);
    return;
  }

  for (int i = 0; i < kChildCount; i++) {
    const intptr_t child_address = 0x1100 + i * 0x100;
    Dart_Handle yoga_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
    if (Dart_IsError(yoga_layout)) {
      ReportDartError("Dart_NewTypedData(child)", yoga_layout);
      return;
    }
    Dart_Handle decorated = newDecoratedBox(
        child_address, colors[i],
        0.0, 0.0, 0.0, 0.0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0,
        Dart_Null(), 0, 0,
        0.0, 0.0, 0.0, 0.0,
        0, 0,
        0.0, 0.0,
        0, 0,
        Dart_Null(), 0,
        Dart_Null(), 0,
        nullptr, nullptr, nullptr, nullptr,
        Dart_Null());
    if (Dart_IsError(decorated)) {
      ReportDartError("NewDecoratedBox(child)", decorated);
      return;
    }

    Dart_Handle child_node = newYGNode(
        child_address, yoga_layout,
        0, 0, 0, 0, 1, Dart_Null(),
        0.0, 0.0, 0.0, 0.0,
        1, 0, 0,
        Dart_Null(), Dart_Null(), decorated);
    if (Dart_IsError(child_node)) {
      ReportDartError("NewYGNode(child)", child_node);
      return;
    }
    Dart_Handle set_status = Dart_ListSetAt(children, i, child_node);
    if (Dart_IsError(set_status)) {
      ReportDartError("Dart_ListSetAt", set_status);
      return;
    }

    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(yoga_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[child_address] = static_cast<float*>(data);
      // AcquireData starts a VM "no callback" scope; release immediately.  The
      // YGNode keeps the Float32List alive and typed-data memory does not move.
      Dart_TypedDataReleaseData(yoga_layout);
    } else {
      std::fprintf(stderr, "[skyline-runtime] demo page: bad child yoga buffer\n");
    }
    g_demo_nodes.push_back(DemoNode{child_address, i + 1});
  }

  Dart_Handle connector = newYGConnector(0, children);
  if (Dart_IsError(connector)) {
    ReportDartError("NewYGConnector", connector);
    return;
  }

  const intptr_t root_address = 0x1000;
  Dart_Handle root_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  if (Dart_IsError(root_layout)) {
    ReportDartError("Dart_NewTypedData(root)", root_layout);
    return;
  }
  Dart_Handle root = newYGNode(
      root_address, root_layout,
      1, 0, 0, 0, 1, Dart_Null(),
      0.0, 0.0, 0.0, 0.0,
      1, 0, 0,
      Dart_Null(), Dart_Null(), connector);
  if (Dart_IsError(root)) {
    ReportDartError("NewYGNode(root)", root);
    return;
  }
  {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(root_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[root_address] = static_cast<float*>(data);
      Dart_TypedDataReleaseData(root_layout);
    } else {
      std::fprintf(stderr, "[skyline-runtime] demo page: bad root yoga buffer\n");
    }
  }
  g_demo_nodes.push_back(DemoNode{root_address, 0});

  static char page_url[] = "pages/index";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = page_url;

  std::fprintf(stderr, "[skyline-runtime] demo page -> CreatePageRoute\n");
  createPageRoute(
      0, 4, 0, 0, 1,
      1, 1, 0, 0,
      0, 0, 0,
      empty,
      1, 1, 0, 0, 0, 0, 0,
      0,
      empty, empty, Dart_Null(), root, &config);
}

void CreateTextPage(RuntimeHolder* holder) {
  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* createPageRoute = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  auto* newText = reinterpret_cast<NewTextFn>(
      FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  if (newYGNode == nullptr || newYGConnector == nullptr ||
      createPageRoute == nullptr || newText == nullptr ||
      newTextSpan == nullptr ||
      (g_demo_mode == DemoMode::kCard && newDecoratedBox == nullptr)) {
    std::fprintf(stderr, "[skyline-runtime] text page: callbacks not registered\n");
    return;
  }

  g_demo_nodes.clear();
  Dart_Handle empty = Dart_NewStringFromCString("");
  Dart_Handle text_handle = Dart_NewStringFromCString("Skyline on Linux");
  Dart_Handle no_children = Dart_NewList(0);
  if (Dart_IsError(text_handle) || Dart_IsError(no_children)) {
    std::fprintf(stderr, "[skyline-runtime] text page: cannot create strings\n");
    return;
  }

  const intptr_t text_node_address = 0x2000;
  const double nan = std::nan("");
  Dart_Handle span = newTextSpan(
      text_node_address, text_handle, 0xFF000000u, 0,
      0, 0, 0, 0.0,
      nullptr, 0,
      32.0, 3 /* FontWeight.w400 */, 0 /* FontStyle.normal */,
      Dart_Null(), nan, nan, nan,
      0 /* WordBreak.normal */, Dart_Null(), no_children);
  if (Dart_IsError(span)) {
    std::fprintf(stderr, "[skyline-runtime] text page: NewTextSpan failed\n");
    return;
  }
  Dart_Handle spans = Dart_NewList(1);
  Dart_ListSetAt(spans, 0, span);
  Dart_Handle text_widget = newText(
      text_node_address,
      reinterpret_cast<void*>(&DemoMarkDirty),
      reinterpret_cast<void*>(&DemoCreateParagraph),
      0 /* TextAlign.left */, 1 /* TextDirection.ltr */, 0 /* clip */,
      1 /* softWrap */, 0 /* maxLines */, 0 /* userSelect */,
      0 /* selectOnGesture */, 0 /* enableNativeParagraph */, spans);
  if (Dart_IsError(text_widget)) {
    std::fprintf(stderr, "[skyline-runtime] text page: NewText failed\n");
    return;
  }

  Dart_Handle node_child = text_widget;
  if (g_demo_mode == DemoMode::kCard) {
    node_child = newDecoratedBox(
        text_node_address, 0xFFF5F5F5u,
        3.0, 3.0, 3.0, 3.0,
        0xFF3366FFu, 0xFF3366FFu, 0xFF3366FFu, 0xFF3366FFu,
        1, 1, 1, 1,
        16.0, 16.0, 16.0, 16.0, 16.0, 16.0, 16.0, 16.0,
        0, 0, 0, 0, 0, 0, 0, 0,
        Dart_Null(), 0, 0,
        0.0, 0.0, 0.0, 0.0,
        0, 0,
        0.0, 0.0,
        0, 0,
        Dart_Null(), 0,
        Dart_Null(), 0,
        nullptr, nullptr, nullptr, nullptr,
        text_widget);
    if (Dart_IsError(node_child)) {
      std::fprintf(stderr, "[skyline-runtime] card page: NewDecoratedBox failed\n");
      return;
    }
  }

  Dart_Handle text_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  Dart_Handle text_node = newYGNode(
      text_node_address, text_layout,
      0, 0, 0, 0, 1, Dart_Null(),
      0.0, 0.0, 0.0, 0.0,
      1, 0, 0,
      Dart_Null(), Dart_Null(), node_child);
  if (Dart_IsError(text_node)) {
    std::fprintf(stderr, "[skyline-runtime] text page: text YGNode failed\n");
    return;
  }
  {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(text_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[text_node_address] = static_cast<float*>(data);
      Dart_TypedDataReleaseData(text_layout);
    }
  }
  g_demo_nodes.push_back(DemoNode{text_node_address, 1});

  Dart_Handle children = Dart_NewList(1);
  Dart_ListSetAt(children, 0, text_node);
  Dart_Handle connector = newYGConnector(0, children);
  if (Dart_IsError(connector)) {
    std::fprintf(stderr, "[skyline-runtime] text page: NewYGConnector failed\n");
    return;
  }

  const intptr_t root_address = 0x1000;
  Dart_Handle root_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  Dart_Handle root = newYGNode(
      root_address, root_layout,
      1, 0, 0, 0, 1, Dart_Null(),
      0.0, 0.0, 0.0, 0.0,
      1, 0, 0,
      Dart_Null(), Dart_Null(), connector);
  if (Dart_IsError(root)) {
    std::fprintf(stderr, "[skyline-runtime] text page: root YGNode failed\n");
    return;
  }
  {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(root_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[root_address] = static_cast<float*>(data);
      Dart_TypedDataReleaseData(root_layout);
    }
  }
  g_demo_nodes.push_back(DemoNode{root_address, 0});

  static char page_url_text[] = "pages/text";
  static char page_url_card[] = "pages/card";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = g_demo_mode == DemoMode::kCard ? page_url_card : page_url_text;

  std::fprintf(stderr, "[skyline-runtime] %s page -> CreatePageRoute\n",
               g_demo_mode == DemoMode::kCard ? "card" : "text");
  createPageRoute(
      0, 4, 0, 0, 1,
      1, 1, 0, 0,
      0, 0, 0,
      empty,
      1, 1, 0, 0, 0, 0, 0,
      0,
      empty, empty, Dart_Null(), root, &config);
}

namespace {

uint32_t ParseColorString(const char* value, uint32_t fallback) {
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  const char* p = value;
  if (*p == '#') {
    p++;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(p, &end, 16);
  if (end == p) {
    return fallback;
  }
  if (std::strlen(p) <= 6) {
    return 0xFF000000u | static_cast<uint32_t>(parsed & 0xFFFFFFu);
  }
  return static_cast<uint32_t>(parsed);
}

Dart_Handle CreateTextWidget(NewTextFn newText,
                             NewTextSpanFn newTextSpan,
                             intptr_t address,
                             const char* value,
                             double fontSize,
                             uint32_t color,
                             int fontWeight,
                             int fontStyle,
                             double letterSpacing,
                             double lineHeight,
                             int textAlign,
                             int maxLines,
                             Dart_Handle* out_span = nullptr) {
  Dart_Handle text_handle = Dart_NewStringFromCString(value);
  Dart_Handle no_children = Dart_NewList(0);
  if (Dart_IsError(text_handle) || Dart_IsError(no_children)) {
    return Dart_IsError(text_handle) ? text_handle : no_children;
  }
  // Public engines do not apply the private TextStyle flags, but they do honour
  // the public `fontFamilyFallback` list.  Listing common CJK/system families
  // keeps Chinese text from falling back to tofu when the host has them.
  Dart_Handle fallback = Dart_Null();
  {
    static const char* kFallbackFamilies[] = {"Noto Sans CJK SC",
                                              "Noto Sans CJK",
                                              "Noto Sans",
                                              "DejaVu Sans"};
    Dart_Handle list = Dart_NewList(
        static_cast<intptr_t>(sizeof(kFallbackFamilies) /
                              sizeof(kFallbackFamilies[0])));
    if (!Dart_IsError(list)) {
      for (size_t i = 0; i < sizeof(kFallbackFamilies) /
                                 sizeof(kFallbackFamilies[0]);
           i++) {
        Dart_Handle family = Dart_NewStringFromCString(kFallbackFamilies[i]);
        if (!Dart_IsError(family)) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), family);
        }
      }
      fallback = list;
    }
  }
  const double nan = std::nan("");
  Dart_Handle span = newTextSpan(
      address, text_handle, color, 0, 0, 0, 0, 0.0, nullptr, 0, fontSize,
      static_cast<uint8_t>(fontWeight), static_cast<uint8_t>(fontStyle),
      fallback, std::isnan(lineHeight) ? nan : lineHeight,
      std::isnan(letterSpacing) ? nan : letterSpacing, nan, 0, Dart_Null(),
      no_children);
  if (Dart_IsError(span)) {
    return span;
  }
  if (out_span != nullptr) *out_span = span;
  Dart_Handle spans = Dart_NewList(1);
  Dart_ListSetAt(spans, 0, span);
  return newText(address, reinterpret_cast<void*>(&DemoMarkDirty),
                 reinterpret_cast<void*>(&DemoCreateParagraph),
                 static_cast<uint8_t>(textAlign), 1 /* ltr */, 0 /* clip */,
                 1 /* softWrap */, static_cast<uint32_t>(maxLines), 0, 0,
                 0 /* Flutter text */, spans);
}

bool ReadDescriptor(const char* descriptor, std::string* out) {  if (descriptor[0] != '@') {
    *out = descriptor;
    return true;
  }
  std::ifstream file(descriptor + 1);
  if (!file) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(file),
              std::istreambuf_iterator<char>());
  return true;
}

float MemberNumber(const rapidjson::Value& object,
                   const char* key,
                   float fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsNumber()) {
    return fallback;
  }
  return static_cast<float>(member->value.GetDouble());
}

const char* MemberString(const rapidjson::Value& object,
                         const char* key,
                         const char* fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsString()) {
    return fallback;
  }
  return member->value.GetString();
}

}  // namespace

namespace {

struct BuiltJsonNode {
  JsonNode layout;
  Dart_Handle widget = nullptr;
};

BuiltJsonNode BuildJsonNode(const rapidjson::Value& entry,
                            int depth,
                            intptr_t address,
                            NewYGNodeFn newYGNode,
                            NewYGConnectorFn newYGConnector,
                            NewTextFn newText,
                            NewTextSpanFn newTextSpan,
                            NewDecoratedBoxFn newDecoratedBox,
                            NewOpacityFn newOpacity,
                            NewClipRRectFn newClipRRect,
                            NewLinearGradientFn newLinearGradient,
                            NewBoxShadowFn newBoxShadow,
                            intptr_t* next_address) {
  BuiltJsonNode built;
  built.layout.address = address;
  built.layout.row =
      std::strcmp(MemberString(entry, "direction", "column"), "row") == 0;
  built.layout.padding = MemberNumber(entry, "padding", 0.0f);
  built.layout.gap = MemberNumber(entry, "gap", 0.0f);

  Dart_Handle connector = nullptr;
  const auto children_member = entry.FindMember("children");
  if (children_member != entry.MemberEnd() && children_member->value.IsArray() &&
      children_member->value.Size() > 0) {
    const rapidjson::Value& entries = children_member->value;
    Dart_Handle list = Dart_NewList(static_cast<intptr_t>(entries.Size()));
    if (Dart_IsError(list)) {
      std::fprintf(stderr, "[skyline-runtime] json page: Dart_NewList failed\n");
      return BuiltJsonNode();
    }
    for (rapidjson::SizeType i = 0; i < entries.Size(); i++) {
      const rapidjson::Value& child_entry = entries[i];
      if (!child_entry.IsObject()) {
        std::fprintf(stderr, "[skyline-runtime] json page: child %u is not an object\n",
                     static_cast<unsigned>(i));
        return BuiltJsonNode();
      }
      const intptr_t child_address = *next_address;
      *next_address += 0x100;
      BuiltJsonNode child = BuildJsonNode(
          child_entry, depth + 1, child_address, newYGNode, newYGConnector,
          newText, newTextSpan, newDecoratedBox, newOpacity, newClipRRect,
          newLinearGradient, newBoxShadow, next_address);
      if (child.widget == nullptr) {
        return BuiltJsonNode();
      }
      if (depth == 0) {
        g_json_root_child_records.push_back(
            RootChildRecord{Dart_NewPersistentHandle(child.widget),
                            child.layout.address});
      }
      built.layout.children.push_back(child.layout);
      built.layout.children.back().flex =
          MemberNumber(child_entry, "flex", 0.0f);
      const auto size_member = child_entry.FindMember("size");
      built.layout.children.back().main_size =
          (size_member != child_entry.MemberEnd() &&
           size_member->value.IsNumber())
              ? static_cast<float>(size_member->value.GetDouble())
              : std::nan("");
      if (Dart_IsError(Dart_ListSetAt(list, static_cast<intptr_t>(i), child.widget))) {
        std::fprintf(stderr, "[skyline-runtime] json page: Dart_ListSetAt failed\n");
        return BuiltJsonNode();
      }
    }
    connector = newYGConnector(0, list);
    if (Dart_IsError(connector)) {
      std::fprintf(stderr, "[skyline-runtime] json page: NewYGConnector failed\n");
      return BuiltJsonNode();
    }
    if (depth == 0) {
      g_json_root_connector = Dart_NewPersistentHandle(connector);
    }
  }

  Dart_Handle inner = connector != nullptr ? connector : Dart_Null();
  const char* type = MemberString(entry, "type", "view");
  if (std::strcmp(type, "opacity") == 0) {
    inner = newOpacity(MemberNumber(entry, "opacity", 1.0f), inner);
    if (Dart_IsError(inner)) {
      std::fprintf(stderr, "[skyline-runtime] json page: NewOpacity failed\n");
      return BuiltJsonNode();
    }
    const auto opacity_update = entry.FindMember("updateOpacity");
    if (opacity_update != entry.MemberEnd() &&
        opacity_update->value.IsObject()) {
      g_pending_style_updates.push_back(PendingStyleUpdate{
          Dart_NewPersistentHandle(inner), StyleUpdateKind::kOpacity,
          MemberNumber(opacity_update->value, "value", 0.5f), 0.0, 0u,
          static_cast<int>(
              MemberNumber(opacity_update->value, "afterMs", 300.0f))});
    }
  } else if (std::strcmp(type, "clip") == 0) {
    const double radius = MemberNumber(entry, "radius", 0.0f);
    const uint8_t disable_clip =
        MemberNumber(entry, "disableClip", 0.0f) > 0 ? 1 : 0;
    inner = newClipRRect(
        disable_clip,
        radius, radius, radius, radius, radius, radius, radius, radius,
        0, 0, 0, 0, 0, 0, 0, 0,
        0.0, 0.0, 0.0, 0.0,
        inner);
    if (Dart_IsError(inner)) {
      std::fprintf(stderr, "[skyline-runtime] json page: NewClipRRect failed\n");
      return BuiltJsonNode();
    }
  } else if (std::strcmp(type, "text") == 0) {
    inner = CreateTextWidget(
        newText, newTextSpan, address, MemberString(entry, "text", "Text"),
        MemberNumber(entry, "fontSize", 20.0f),
        ParseColorString(MemberString(entry, "color", "#000000"), 0xFF000000u),
        static_cast<int>(MemberNumber(entry, "fontWeight", 3.0f)),
        static_cast<int>(MemberNumber(entry, "fontStyle", 0.0f)),
        MemberNumber(entry, "letterSpacing", std::nan("")),
        MemberNumber(entry, "lineHeight", std::nan("")),
        static_cast<int>(MemberNumber(entry, "textAlign", 0.0f)),
        static_cast<int>(MemberNumber(entry, "maxLines", 0.0f)));
    if (Dart_IsError(inner)) {
      std::fprintf(stderr, "[skyline-runtime] json page: text widget failed\n");
      return BuiltJsonNode();
    }
  } else {
    const bool has_visual =
        entry.HasMember("color") || entry.HasMember("borderColor") ||
        entry.HasMember("borderWidth") || entry.HasMember("radius") ||
        entry.HasMember("gradient") || entry.HasMember("shadow");

    Dart_Handle gradient_handle = Dart_Null();
    const auto gradient_member = entry.FindMember("gradient");
    if (gradient_member != entry.MemberEnd() &&
        gradient_member->value.IsObject()) {
      const rapidjson::Value& gradient = gradient_member->value;
      const auto colors_member = gradient.FindMember("colors");
      if (colors_member != gradient.MemberEnd() &&
          colors_member->value.IsArray() &&
          colors_member->value.Size() > 0) {
        const rapidjson::Value& colors = colors_member->value;
        Dart_Handle color_list =
            Dart_NewList(static_cast<intptr_t>(colors.Size()));
        std::vector<GradientStopNative> stops;
        const auto stops_member = gradient.FindMember("stops");
        for (rapidjson::SizeType i = 0; i < colors.Size(); i++) {
          const char* color_string =
              colors[i].IsString() ? colors[i].GetString() : "#000000";
          Dart_ListSetAt(color_list, static_cast<intptr_t>(i),
                         Dart_NewInteger(static_cast<int64_t>(
                             ParseColorString(color_string, 0xFF000000u))));
          double value = colors.Size() > 1
                             ? static_cast<double>(i) / (colors.Size() - 1)
                             : 0.0;
          if (stops_member != gradient.MemberEnd() &&
              stops_member->value.IsArray() &&
              i < stops_member->value.Size() &&
              stops_member->value[i].IsNumber()) {
            value = stops_member->value[i].GetDouble();
          }
          stops.push_back(GradientStopNative{value, 1});
        }
        gradient_handle = newLinearGradient(
            MemberNumber(gradient, "angle", 0.0f), color_list, stops.data(),
            -1.0, -1.0 /* FittingSize.none -> fill the box */, 0, 0, 0, 0,
            0.0, 0.0, 0.0, 0.0, 0);
        if (Dart_IsError(gradient_handle)) {
          std::fprintf(stderr, "[skyline-runtime] json page: NewLinearGradient failed\n");
          return BuiltJsonNode();
        }
      }
    }

    Dart_Handle shadows_handle = Dart_Null();
    const auto shadow_member = entry.FindMember("shadow");
    if (shadow_member != entry.MemberEnd() && shadow_member->value.IsObject()) {
      const rapidjson::Value& shadow = shadow_member->value;
      Dart_Handle box_shadow = newBoxShadow(
          ParseColorString(MemberString(shadow, "color", "#00000080"),
                           0x80000000u),
          MemberNumber(shadow, "offsetX", 0.0f),
          MemberNumber(shadow, "offsetY", 0.0f),
          MemberNumber(shadow, "blur", 0.0f),
          MemberNumber(shadow, "spread", 0.0f),
          MemberNumber(shadow, "inset", 0.0f) > 0 ? 1 : 0);
      if (Dart_IsError(box_shadow)) {
        std::fprintf(stderr, "[skyline-runtime] json page: NewBoxShadow failed\n");
        return BuiltJsonNode();
      }
      Dart_Handle shadow_list = Dart_NewList(1);
      Dart_ListSetAt(shadow_list, 0, box_shadow);
      shadows_handle = shadow_list;
    }

    if (has_visual) {
      const uint32_t background =
          ParseColorString(MemberString(entry, "color", "#00000000"), 0u);
      const uint32_t border =
          ParseColorString(MemberString(entry, "borderColor", ""), 0u);
      const double border_width = MemberNumber(entry, "borderWidth", 0.0f);
      const double radius = MemberNumber(entry, "radius", 0.0f);
      const uint8_t border_style = border_width > 0 ? 1 : 0;
      inner = newDecoratedBox(
          address, background,
          border_width, border_width, border_width, border_width,
          border, border, border, border,
          border_style, border_style, border_style, border_style,
          radius, radius, radius, radius, radius, radius, radius, radius,
          0, 0, 0, 0, 0, 0, 0, 0,
          Dart_Null(), 0, 0,
          0.0, 0.0, 0.0, 0.0,
          0, 0,
          0.0, 0.0,
          0, 0,
          gradient_handle, 0,
          shadows_handle, 0,
          nullptr, nullptr, nullptr, nullptr,
          inner);
      if (Dart_IsError(inner)) {
        std::fprintf(stderr, "[skyline-runtime] json page: DecoratedBox failed\n");
        return BuiltJsonNode();
      }
      const auto update_member = entry.FindMember("updateColor");
      if (update_member != entry.MemberEnd() && update_member->value.IsString()) {
        g_pending_color_updates.push_back(PendingColorUpdate{
            Dart_NewPersistentHandle(inner),
            ParseColorString(update_member->value.GetString(), 0xFFFFFFFFu),
            static_cast<int>(MemberNumber(entry, "updateAfterMs", 300.0f))});
      }
      const auto border_update = entry.FindMember("updateBorder");
      if (border_update != entry.MemberEnd() &&
          border_update->value.IsObject()) {
        g_pending_style_updates.push_back(PendingStyleUpdate{
            Dart_NewPersistentHandle(inner), StyleUpdateKind::kBorder, 0.0,
            MemberNumber(border_update->value, "width", 4.0f),
            ParseColorString(
                MemberString(border_update->value, "color", "#FFFF00"),
                0xFFFFFF00u),
            static_cast<int>(
                MemberNumber(border_update->value, "afterMs", 300.0f))});
      }
    }
  }

  Dart_Handle buffer = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  if (Dart_IsError(buffer)) {
    std::fprintf(stderr, "[skyline-runtime] json page: typed data failed\n");
    return BuiltJsonNode();
  }
  Dart_Handle node = newYGNode(
      address, buffer, depth == 0 ? 1 : 0, 0, 0, 0, 1, Dart_Null(), 0.0, 0.0,
      0.0, 0.0, 1, 0, 0, Dart_Null(), Dart_Null(), inner);
  if (Dart_IsError(node)) {
    std::fprintf(stderr, "[skyline-runtime] json page: NewYGNode failed\n");
    return BuiltJsonNode();
  }

  Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
  void* data = nullptr;
  intptr_t length = 0;
  Dart_Handle status =
      Dart_TypedDataAcquireData(buffer, &data_type, &data, &length);
  if (!Dart_IsError(status) && data_type == Dart_TypedData_kFloat32 &&
      length == 9 && data != nullptr) {
    g_yoga_metrics[address] = static_cast<float*>(data);
    Dart_TypedDataReleaseData(buffer);
  }
  built.widget = node;
  return built;
}

void LayoutJsonNode(const JsonNode& node,
                    float left,
                    float top,
                    float width,
                    float height) {
  const auto found = g_yoga_metrics.find(node.address);
  if (found != g_yoga_metrics.end() && found->second != nullptr) {
    float* m = found->second;
    m[0] = top;
    m[1] = left;
    m[2] = width;
    m[3] = height;
    for (int k = 4; k < 9; k++) m[k] = 0.0f;
  }
  if (node.children.empty()) {
    return;
  }
  // Child offsets are stored relative to this node (the YGNode render object
  // writes boxParentData.offset from the buffer), not relative to the viewport.
  const float content_left = node.padding;
  const float content_top = node.padding;
  const float content_width =
      width - 2 * node.padding > 0 ? width - 2 * node.padding : 0.0f;
  const float content_height =
      height - 2 * node.padding > 0 ? height - 2 * node.padding : 0.0f;
  const size_t count = node.children.size();
  const float main_extent =
      (node.row ? content_width : content_height) -
      (count > 1 ? node.gap * static_cast<float>(count - 1) : 0.0f);
  float fixed_sum = 0.0f;
  float flex_sum = 0.0f;
  for (const JsonNode& child : node.children) {
    if (std::isnan(child.main_size)) {
      flex_sum += child.flex;
    } else {
      fixed_sum += child.main_size;
    }
  }
  float remaining = main_extent - fixed_sum;
  if (remaining < 0.0f) remaining = 0.0f;
  float cursor = node.row ? content_left : content_top;
  for (const JsonNode& child : node.children) {
    const float main = std::isnan(child.main_size)
                           ? (flex_sum > 0.0f ? remaining * child.flex / flex_sum : 0.0f)
                           : child.main_size;
    if (node.row) {
      LayoutJsonNode(child, cursor, content_top, main, content_height);
    } else {
      LayoutJsonNode(child, content_left, cursor, content_width, main);
    }
    cursor += main + node.gap;
  }
}

}  // namespace

void CreateJsonPage(RuntimeHolder* holder, const char* descriptor) {
  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* createPageRoute = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  auto* newText = reinterpret_cast<NewTextFn>(
      FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newClipRRect = reinterpret_cast<NewClipRRectFn>(
      FindCallback(holder->ffi_table, kNewClipRRectHash));
  auto* newLinearGradient = reinterpret_cast<NewLinearGradientFn>(
      FindCallback(holder->ffi_table, kNewLinearGradientHash));
  auto* newBoxShadow = reinterpret_cast<NewBoxShadowFn>(
      FindCallback(holder->ffi_table, kNewBoxShadowHash));
  auto* updateColor = reinterpret_cast<DecoratedBoxUpdateColorFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateColorHash));
  g_update_opacity = reinterpret_cast<OpacityUpdateFn>(
      FindCallback(holder->ffi_table, kOpacityUpdateHash));
  g_update_border = reinterpret_cast<DecoratedBoxUpdateBorderFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateBorderHash));
  auto* updateChildren = reinterpret_cast<ConnectorUpdateChildrenFn>(
      FindCallback(holder->ffi_table, kYGConnectorUpdateChildrenHash));
  auto* markNeedsLayout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  if (newYGNode == nullptr || newYGConnector == nullptr ||
      createPageRoute == nullptr || newText == nullptr ||
      newTextSpan == nullptr || newDecoratedBox == nullptr ||
      newOpacity == nullptr || newClipRRect == nullptr ||
      newLinearGradient == nullptr || newBoxShadow == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] json page: callbacks not registered\n");
    return;
  }

  g_pending_color_updates.clear();
  g_pending_style_updates.clear();
  g_json_root_connector = nullptr;
  g_json_root_child_records.clear();

  std::string json;
  if (!ReadDescriptor(descriptor, &json)) {
    std::fprintf(stderr, "[skyline-runtime] json page: cannot read %s\n", descriptor);
    return;
  }
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    std::fprintf(stderr, "[skyline-runtime] json page: parse error at %zu\n",
                 static_cast<size_t>(doc.GetErrorOffset()));
    return;
  }

  intptr_t next_address = 0x1100;
  BuiltJsonNode root = BuildJsonNode(doc, 0, 0x1000, newYGNode, newYGConnector,
                                     newText, newTextSpan, newDecoratedBox,
                                     newOpacity, newClipRRect, newLinearGradient,
                                     newBoxShadow, &next_address);
  if (root.widget == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] json page: build failed\n");
    return;
  }
  g_json_root = root.layout;
  g_json_next_address = next_address;

  static char page_url[] = "pages/json";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = page_url;
  Dart_Handle empty = Dart_NewStringFromCString("");

  std::fprintf(stderr, "[skyline-runtime] json page -> CreatePageRoute\n");
  createPageRoute(0, 4, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, empty, 1, 1, 0, 0, 0, 0,
                  0, 0, empty, empty, Dart_Null(), root.widget, &config);
  SchedulePendingStyleUpdates();

  if (updateColor != nullptr && !g_pending_color_updates.empty()) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
#pragma GCC diagnostic pop
    if (task_runner) {
      for (const PendingColorUpdate& update : g_pending_color_updates) {
        std::fprintf(stderr,
                     "[skyline-runtime] json page: color update in %d ms\n",
                     update.delay_ms);
        task_runner->PostDelayedTask(
            [updateColor, update]() {
              if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
                return;
              }
              DemoPageApiScope scope;
              Dart_Handle decorated =
                  Dart_HandleFromPersistent(update.decorated_box);
              updateColor(decorated, update.color);
              Dart_DeletePersistentHandle(update.decorated_box);
            },
            fml::TimeDelta::FromMilliseconds(update.delay_ms));
      }
    }
  }

  const auto ops_member = doc.FindMember("ops");
  if (ops_member != doc.MemberEnd() && ops_member->value.IsArray()) {
    auto task_runner = UiTaskRunner();
    if (task_runner) {
      const rapidjson::Value& ops = ops_member->value;
      for (rapidjson::SizeType i = 0; i < ops.Size(); i++) {
        const rapidjson::Value& op = ops[i];
        if (!op.IsObject()) {
          continue;
        }
        const char* kind = MemberString(op, "op", "");
        const int delay_ms =
            static_cast<int>(MemberNumber(op, "afterMs", 300.0f));
        if (std::strcmp(kind, "append") == 0 && updateChildren != nullptr) {
          const auto child_member = op.FindMember("child");
          if (child_member == op.MemberEnd() || !child_member->value.IsObject()) {
            continue;
          }
          rapidjson::StringBuffer buffer;
          rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
          child_member->value.Accept(writer);
          const std::string child_json(buffer.GetString(), buffer.GetLength());
          task_runner->PostDelayedTask(
              [child_json, updateChildren, newYGNode, newYGConnector, newText,
               newTextSpan, newDecoratedBox, newOpacity, newClipRRect,
               newLinearGradient, newBoxShadow, markNeedsLayout]() {
                if (g_json_root_connector == nullptr ||
                    (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr)) {
                  return;
                }
                DemoPageApiScope scope;
                rapidjson::Document doc;
                doc.Parse(child_json.c_str());
                if (doc.HasParseError() || !doc.IsObject()) {
                  std::fprintf(stderr,
                               "[skyline-runtime] append op: bad child json\n");
                  return;
                }
                const intptr_t address = g_json_next_address;
                g_json_next_address += 0x100;
                BuiltJsonNode child = BuildJsonNode(
                    doc, 1, address, newYGNode, newYGConnector, newText,
                    newTextSpan, newDecoratedBox, newOpacity, newClipRRect,
                    newLinearGradient, newBoxShadow, &g_json_next_address);
                if (child.widget == nullptr) {
                  return;
                }
                child.layout.flex =
                    static_cast<float>(MemberNumber(doc, "flex", 0.0f));
                const auto size_member = doc.FindMember("size");
                child.layout.main_size =
                    (size_member != doc.MemberEnd() &&
                     size_member->value.IsNumber())
                        ? static_cast<float>(size_member->value.GetDouble())
                        : std::nan("");
                SchedulePendingStyleUpdates();
                Dart_PersistentHandle persistent =
                    Dart_NewPersistentHandle(child.widget);
                g_json_root.children.push_back(child.layout);
                g_json_root_child_records.push_back(
                    RootChildRecord{persistent, child.layout.address});
                Dart_Handle list = Dart_NewList(
                    static_cast<intptr_t>(g_json_root_child_records.size()));
                for (intptr_t i = 0;
                     i < static_cast<intptr_t>(g_json_root_child_records.size());
                     i++) {
                  Dart_ListSetAt(
                      list, i,
                      Dart_HandleFromPersistent(
                          g_json_root_child_records[i].handle));
                }
                Dart_Handle connector =
                    Dart_HandleFromPersistent(g_json_root_connector);
                updateChildren(connector, 0, list);
                if (markNeedsLayout != nullptr) {
                  for (const RootChildRecord& item :
                       g_json_root_child_records) {
                    markNeedsLayout(Dart_HandleFromPersistent(item.handle));
                  }
                }
              },
              fml::TimeDelta::FromMilliseconds(delay_ms));
          std::fprintf(stderr,
                       "[skyline-runtime] json page: append child in %d ms\n",
                       delay_ms);
        } else if (std::strcmp(kind, "remove") == 0 &&
                   updateChildren != nullptr) {
          const int index =
              static_cast<int>(MemberNumber(op, "index", 0.0f));
          if (index < 0 ||
              index >= static_cast<int>(g_json_root_child_records.size())) {
            continue;
          }
          const intptr_t address = g_json_root_child_records[index].address;
          task_runner->PostDelayedTask(
              [updateChildren, markNeedsLayout, address]() {
                if (g_json_root_connector == nullptr ||
                    (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr)) {
                  return;
                }
                DemoPageApiScope scope;
                const auto record = std::find_if(
                    g_json_root_child_records.begin(),
                    g_json_root_child_records.end(),
                    [address](const RootChildRecord& item) {
                      return item.address == address;
                    });
                if (record == g_json_root_child_records.end()) {
                  return;
                }
                const size_t remaining =
                    g_json_root_child_records.size() - 1;
                Dart_Handle list =
                    Dart_NewList(static_cast<intptr_t>(remaining));
                intptr_t index = 0;
                for (const RootChildRecord& item : g_json_root_child_records) {
                  if (item.address == address) {
                    continue;
                  }
                  Dart_ListSetAt(list, index++,
                                 Dart_HandleFromPersistent(item.handle));
                }
                Dart_PersistentHandle persistent = record->handle;
                g_json_root.children.erase(
                    std::remove_if(
                        g_json_root.children.begin(), g_json_root.children.end(),
                        [address](const JsonNode& node) {
                          return node.address == address;
                        }),
                    g_json_root.children.end());
                g_json_root_child_records.erase(record);
                Dart_DeletePersistentHandle(persistent);
                Dart_Handle connector =
                    Dart_HandleFromPersistent(g_json_root_connector);
                updateChildren(connector, 0, list);
                if (markNeedsLayout != nullptr) {
                  for (const RootChildRecord& item :
                       g_json_root_child_records) {
                    markNeedsLayout(Dart_HandleFromPersistent(item.handle));
                  }
                }
              },
              fml::TimeDelta::FromMilliseconds(delay_ms));
          std::fprintf(stderr,
                       "[skyline-runtime] json page: remove child in %d ms\n",
                       delay_ms);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Appservice / V8 node-operation replay (embedded build)
//
// The official renderer runs `skyline-addon/bundle/flutter_assets/assets/app.js`
// inside its embedded V8 build.  That JS bundle (exparser + skyline-js) emits a
// stream of page-context operations to native objects:
//
//   context.createElement(tag, name)      -> NativeNode
//   node.setText / setAttribute / setStyle / addClass / appendChild / ...
//   context.appendStyleSheet(css, index)
//   context.attach(root) ; context.startRender()
//
// `tools/record-skyline-ops.js` runs the same bundle in Node's V8 with a
// capture shim and writes that operation stream as JSON.  This section replays
// the stream through the real `package:skyline` callbacks registered in the FFI
// table, replacing the earlier hand-written JSON widget descriptors.
//
// Scope: static pages.  Geometry is computed by a small flexbox-subset layout
// driven by the CSS the renderer appends (classes, inline styles, inheritance).
// ---------------------------------------------------------------------------

namespace appservice {

constexpr float kInf = 1e30f;

// CSS transition properties modelled by the replay runtime.  The bit mask is
// stored on AppStyle so a style change can start the matching animations.
enum TransitionProperty : uint32_t {
  kTransitionOpacity = 1u << 0,
  kTransitionBackground = 1u << 1,
  kTransitionColor = 1u << 2,
  kTransitionBorderColor = 1u << 3,
  kTransitionTransform = 1u << 4,
};

struct AppStyle {
  bool row = false;
  int justify = 0;  // 0 start, 1 center, 2 end, 3 space-between
  int align = 0;    // 0 stretch, 1 start, 2 center, 3 end
  int align_self = -1;  // -1 auto, otherwise same values as `align`
  bool wrap = false;
  int align_content = 1;  // 0 stretch, 1 start, 2 center, 3 end
  int order = 0;
  float padding[4] = {0, 0, 0, 0};  // top, right, bottom, left
  float margin[4] = {0, 0, 0, 0};
  float gap = 0.0f;
  float row_gap = 0.0f;
  float column_gap = 0.0f;

  // CSS position/insets.  position == 1 absolute, 2 fixed: the node is removed
  // from flex flow; absolute/fixed insets resolve against the parent content box
  // or the viewport respectively.
  int position = 0;
  float inset[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                    std::nanf("")};  // top, right, bottom, left (px)
  float inset_percent[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                            std::nanf("")};
  int z_index = 0;
  // CSS transform (column-major 4x4, applied around the element centre).
  bool has_transform = false;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                         0, 0, 1, 0, 0, 0, 0, 1};
  // CSS transform-origin (px or percent; NaN = unset, default 50% 50%).
  float transform_origin_x = std::nanf("");
  float transform_origin_y = std::nanf("");
  float transform_origin_x_percent = std::nanf("");
  float transform_origin_y_percent = std::nanf("");
  // CSS transitions.  `transition_mask` selects the animated properties.
  uint32_t transition_mask = 0;
  double transition_duration = 0.0;  // ms
  double transition_delay = 0.0;     // ms
  int transition_easing = 0;         // 0 linear, 1 ease, 2 ease-in, 3 ease-out
                                     // 4 ease-in-out
  // First CSS box-shadow layer.
  bool has_shadow = false;
  uint32_t shadow_color = 0u;
  float shadow_offset_x = 0.0f;
  float shadow_offset_y = 0.0f;
  float shadow_blur = 0.0f;
  float shadow_spread = 0.0f;
  bool shadow_inset = false;

  // CSS background image (url(...)); painted behind the node's children.
  std::string background_image;
  int background_repeat = 0;  // 0 no-repeat, 1 repeat, 2 repeat-x, 3 repeat-y
  int background_fit = 0;     // 0 fill, 1 contain, 2 cover
  // First CSS linear-gradient layer.
  bool has_gradient = false;
  double gradient_angle = 0.0;  // radians
  std::vector<uint32_t> gradient_colors;
  std::vector<double> gradient_stops;

  float width = -1.0f;          // px, -1 = auto
  float height = -1.0f;
  float width_percent = -1.0f;  // 0..1
  float height_percent = -1.0f;
  float min_width = 0.0f;
  float max_width = kInf;
  float min_height = 0.0f;
  float max_height = kInf;
  float flex = 0.0f;
  bool has_flex = false;

  uint32_t background = 0u;
  bool has_background = false;
  uint32_t border_color = 0u;
  float border_width = 0.0f;
  // Per-side overrides (top, right, bottom, left); -1 = use the uniform value.
  float side_border_width[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  uint32_t side_border_color[4] = {0u, 0u, 0u, 0u};
  bool side_border_color_set[4] = {false, false, false, false};
  float radius = 0.0f;
  // Per-corner radii (TL, TR, BR, BL); -1 = use the uniform `radius`.
  float corner_radius[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  bool corner_radius_percent[4] = {false, false, false, false};
  bool corner_radius_ellipse[4] = {false, false, false, false};
  double opacity = 1.0;

  double font_size = 16.0;
  uint32_t color = 0xFF000000u;
  int font_weight = 3;  // FontWeight.w400
  int font_style = 0;   // normal
  double line_height = -1.0;         // absolute px, -1 = use factor
  double line_height_factor = 1.2;   // multiplier
  double letter_spacing = std::nan("");
  int text_align = 0;
  uint32_t max_lines = 0;
  bool display_none = false;
  bool clip = false;  // overflow: hidden/clip
  // Image fit: 0 fill, 1 contain, 2 cover, 3 none, 4 scale-down.
  int object_fit = 0;
};

struct AppNode {
  uint32_t id = 0;
  std::string tag;
  std::string name;
  std::string text;
  std::vector<std::string> classes;
  std::string inline_style;
  std::unordered_map<std::string, std::string> attrs;
  std::vector<AppNode*> children;
  AppNode* parent = nullptr;
  bool released = false;

  AppStyle style;
  float measured_w = 0.0f;
  float measured_h = 0.0f;
  // Absolute border-box origin relative to the viewport (set by LayoutNode).
  float abs_x = 0.0f;
  float abs_y = 0.0f;
  intptr_t address = 0;
  Dart_Handle widget = nullptr;
  Dart_Handle layout_buffer = nullptr;
  // Widget handles produced during the current materialization (local scope).
  Dart_Handle connector = nullptr;
  Dart_Handle decorated_box = nullptr;
  Dart_Handle text_widget = nullptr;
  Dart_Handle opacity_widget = nullptr;
  Dart_Handle clip_widget = nullptr;

  // Persistent handles keep the attached tree addressable across update tasks.
  Dart_PersistentHandle layout_buffer_p = nullptr;
  Dart_PersistentHandle decorated_box_p = nullptr;
  Dart_PersistentHandle opacity_widget_p = nullptr;
  Dart_PersistentHandle clip_widget_p = nullptr;
  Dart_PersistentHandle text_widget_p = nullptr;
  Dart_PersistentHandle text_span_p = nullptr;
  Dart_PersistentHandle yoga_node_p = nullptr;
  Dart_PersistentHandle scroll_view_p = nullptr;
  // Float64List(16) backing YGNodeSetupTransform while the Matrix4 reads it.
  Dart_PersistentHandle transform_p = nullptr;
  // Decoded image backing a `<image>` node's direct-render paint callback.
  sk_sp<flutter::DlImage> image_dl;
  // Decoded CSS background-image (painted by the decorated box).
  sk_sp<flutter::DlImage> background_image_dl;
  std::string image_source;
  uint32_t image_request_id = 0;
  bool image_target_background = false;
  bool image_loaded = false;
  bool image_failed = false;
  bool image_pending = false;

  // Synthetic input state for `name == "input"` nodes.  Indices are UTF-8
  // codepoint offsets (the replay text model, not Dart UTF-16 offsets).
  int input_cursor = 0;
  int input_selection_start = 0;
  int input_selection_end = 0;
  bool input_hold_keyboard = false;
  std::string composition_text;
  int composition_start = 0;
  int composition_end = 0;
  bool composing = false;
  std::string composition_base_text;
  Dart_Handle input_widget = nullptr;
  Dart_PersistentHandle input_widget_p = nullptr;

  // CSS transition override.  When active, ResolveStyles keeps the animated
  // style instead of the cascade target until the transition completes.
  bool animation_override_active = false;
  AppStyle animation_override;

  // List/grid builder state (NewListBuilder/NewGridBuilder).  Items are
  // materialized on demand from the captured node table when Flutter's sliver
  // asks for an index via the onBuildChild callback.
  Dart_PersistentHandle builder_p = nullptr;
  uint8_t builder_kind = 0;  // 0 = list, 1 = grid
  int builder_child_count = 0;
  int builder_cross_axis_count = 1;
  float builder_child_height = 0.0f;
  float builder_main_spacing = 0.0f;
  float builder_cross_spacing = 0.0f;
  float builder_max_cross_extent = 0.0f;
  uint32_t builder_background = 0u;
  // index -> materialized SkylineListBuilderItem/SkylineGridBuilderItem.
  std::unordered_map<int, Dart_PersistentHandle> builder_items;
  std::unordered_map<int, AppNode*> builder_item_nodes;
  std::unordered_set<int> builder_pending;
  // Last synced metadata and content signature, used for fine-grained updates.
  int builder_synced_child_count = 0;
  float builder_synced_child_height = 0.0f;
  int builder_synced_cross_axis_count = 1;
  float builder_synced_main_spacing = 0.0f;
  float builder_synced_cross_spacing = 0.0f;
  float builder_synced_max_cross_extent = 0.0f;
  uint32_t builder_synced_background = 0u;
  std::unordered_map<int, uint64_t> builder_item_signatures;


  AppStyle previous_style;
  bool has_previous_style = false;
  std::string previous_text;
};

// Nodes with a live native image, keyed by their YGNode address.  The paint
// callback only receives that address.
std::unordered_map<intptr_t, AppNode*> g_image_nodes;

// Nodes whose decorated box paints a CSS background-image.
std::unordered_map<intptr_t, AppNode*> g_background_image_nodes;

// Decoded image cache shared by <image>, CSS backgrounds and SVG results.
// Deliberately leaked: DlImageGPU created with a null unref queue asserts when
// destroyed, and the replay runtime lives for the process lifetime.
std::unordered_map<std::string, sk_sp<flutter::DlImage>>& ImageCache() {
  static auto* cache =
      new std::unordered_map<std::string, sk_sp<flutter::DlImage>>();
  return *cache;
}

// Replaced images are retired instead of destroyed: DlImageGPU holds a
// SkiaGPUObject with a null unref queue, and its destructor asserts.
std::vector<sk_sp<flutter::DlImage>>& RetiredImages() {
  static auto* retired = new std::vector<sk_sp<flutter::DlImage>>();
  return *retired;
}

void CacheImage(const std::string& key, const sk_sp<flutter::DlImage>& image) {
  auto& cache = ImageCache();
  if (image && cache.find(key) == cache.end()) cache.emplace(key, image);
}

// Builder nodes, keyed by YGNode address (the Dart callback only receives the
// address).  Declared here because teardown clears the entry.
std::unordered_map<intptr_t, AppNode*> g_builders_by_address;

// `position: fixed` nodes are collected during the page build and appended to
// the page root connector, so an ancestor scroll does not move them.
std::vector<AppNode*> g_deferred_fixed_nodes;
std::unordered_set<AppNode*> g_deferred_fixed_set;
bool g_collect_fixed_nodes = false;
// The attach-root node whose connector receives promoted fixed nodes.
AppNode* g_fixed_root_node = nullptr;

void ResetDeferredFixedNodes() {
  g_deferred_fixed_nodes.clear();
  g_deferred_fixed_set.clear();
  g_fixed_root_node = nullptr;
}

// Returns true when the child was promoted out of its parent's connector.
bool DeferFixedChild(AppNode* child) {
  if (!g_collect_fixed_nodes || child == nullptr ||
      child->style.position != 2) {
    return false;
  }
  if (g_deferred_fixed_set.insert(child).second) {
    g_deferred_fixed_nodes.push_back(child);
  }
  return true;
}

struct CssRule {
  std::string selector;
  std::vector<std::pair<std::string, std::string>> declarations;
  int order = 0;
};

struct FontFaceRequest {
  std::string family;
  std::string url;
};

// One sampled frame of a JS `addAnimatedStyle` worklet.  The recorder stores
// declarations (CSS syntax) so the replay runtime can apply them without a V8.
struct PendingAnimatedStyle {
  uint32_t node_id = 0;
  int at_ms = 0;
  std::string declarations;
};

struct OpPage {
  std::unordered_map<uint32_t, AppNode*> nodes;
  std::vector<CssRule> rules;
  std::vector<FontFaceRequest> fonts;
  std::vector<PendingAnimatedStyle> animated_styles;
  size_t scheduled_animated_styles = 0;
  uint32_t root_id = 0;
  int applied_ops = 0;
  bool started = false;
  bool force_rebuild = false;

  // Route stack replay: `notifyNavigateTo`/`notifySwitchTab` record the open
  // type used by the next `attach`; each attach materializes one page route.
  uint8_t pending_open_type = 0;
  int32_t pending_page_id = 0;
  int32_t pending_callback_id = 0;
  int pending_back_delta = 1;
  bool pending_back_animated = true;
  int page_count = 0;
  // Synthetic input focus used by the host keyboard channel.
  uint32_t focused_input_id = 0;
  double keyboard_height = 0.0;

  // Attached page identity, used to push incremental updates.
  Dart_PersistentHandle root_node = nullptr;
  Dart_PersistentHandle root_connector = nullptr;
  RuntimeHolder* holder = nullptr;
};

OpPage* g_page = nullptr;
// Captured on the UI thread; the host keyboard channel is called from the
// embedder thread and must marshal work back onto the UI runner.
fml::RefPtr<fml::TaskRunner> g_ui_task_runner;
std::unique_ptr<rapidjson::Document> g_ops_document;
int g_next_rule_order = 0;
intptr_t g_next_address = 0x5000;

// -- text helpers ------------------------------------------------------------

std::string TrimAscii(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) begin++;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
  return value.substr(begin, end - begin);
}

std::string LowerAscii(const std::string& value) {
  std::string out = value;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> SplitWhitespace(const std::string& value) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) i++;
    const size_t begin = i;
    while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) i++;
    if (i > begin) out.push_back(value.substr(begin, i - begin));
  }
  return out;
}

std::vector<std::string> SplitChar(const std::string& value, char separator) {
  std::vector<std::string> out;
  std::string current;
  for (char c : value) {
    if (c == separator) {
      out.push_back(TrimAscii(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  out.push_back(TrimAscii(current));
  return out;
}

bool ParseNumber(const std::string& value, double* out) {
  if (value.empty()) return false;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str()) return false;
  *out = parsed;
  return true;
}

// Parses "12", "12px" or "0".  Percentages are reported separately.
bool ParseLength(const std::string& raw, float* px, float* percent) {
  const std::string value = TrimAscii(raw);
  if (value.empty()) return false;
  if (value == "auto") {
    *px = -1.0f;
    *percent = -1.0f;
    return true;
  }
  if (EndsWith(value, "%")) {
    double number = 0.0;
    if (!ParseNumber(value.substr(0, value.size() - 1), &number)) return false;
    *percent = static_cast<float>(number / 100.0);
    *px = -1.0f;
    return true;
  }
  double number = 0.0;
  std::string numeric = value;
  if (EndsWith(numeric, "px")) numeric = numeric.substr(0, numeric.size() - 2);
  if (!ParseNumber(numeric, &number)) return false;
  *px = static_cast<float>(number);
  *percent = -1.0f;
  return true;
}

uint32_t NamedColor(const std::string& name) {
  if (name == "transparent") return 0x00000000u;
  if (name == "black") return 0xFF000000u;
  if (name == "white") return 0xFFFFFFFFu;
  if (name == "red") return 0xFFFF0000u;
  if (name == "green") return 0xFF008000u;
  if (name == "blue") return 0xFF0000FFu;
  if (name == "yellow") return 0xFFFFFF00u;
  if (name == "orange") return 0xFFFFA500u;
  if (name == "gray" || name == "grey") return 0xFF808080u;
  if (name == "pink") return 0xFFFFC0CBu;
  if (name == "purple") return 0xFF800080u;
  if (name == "cyan") return 0xFF00FFFFu;
  if (name == "magenta") return 0xFFFF00FFu;
  return 0u;
}

bool ParseCssColor(const std::string& raw, uint32_t* out) {
  const std::string value = TrimAscii(raw);
  if (value.empty()) return false;
  if (value[0] == '#') {
    std::string hex = value.substr(1);
    if (hex.size() == 3) {
      hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
    }
    if (hex.size() == 6 || hex.size() == 8) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(hex.c_str(), &end, 16);
      if (end == hex.c_str()) return false;
      *out = hex.size() == 6 ? (0xFF000000u | static_cast<uint32_t>(parsed))
                             : static_cast<uint32_t>(parsed);
      return true;
    }
    return false;
  }
  if (StartsWith(value, "rgb(") || StartsWith(value, "rgba(")) {
    const size_t open = value.find('(');
    const size_t close = value.find(')');
    if (open == std::string::npos || close == std::string::npos) return false;
    const std::vector<std::string> parts =
        SplitChar(value.substr(open + 1, close - open - 1), ',');
    if (parts.size() < 3) return false;
    double r = 0, g = 0, b = 0, a = 255;
    if (!ParseNumber(parts[0], &r) || !ParseNumber(parts[1], &g) ||
        !ParseNumber(parts[2], &b)) {
      return false;
    }
    if (parts.size() > 3) ParseNumber(parts[3], &a);
    if (a <= 1.0) a *= 255.0;
    auto clamp = [](double v) {
      if (v < 0) return 0.0;
      if (v > 255) return 255.0;
      return v;
    };
    *out = (static_cast<uint32_t>(clamp(a)) << 24) |
           (static_cast<uint32_t>(clamp(r)) << 16) |
           (static_cast<uint32_t>(clamp(g)) << 8) |
           static_cast<uint32_t>(clamp(b));
    return true;
  }
  const uint32_t named = NamedColor(LowerAscii(value));
  if (named != 0u || LowerAscii(value) == "transparent") {
    *out = named;
    return true;
  }
  return false;
}

int ParseJustify(const std::string& value) {
  if (value == "center") return 1;
  if (value == "flex-end" || value == "end") return 2;
  if (value == "space-between") return 3;
  return 0;
}

int ParseAlign(const std::string& value) {
  if (value == "flex-start" || value == "start") return 1;
  if (value == "center") return 2;
  if (value == "flex-end" || value == "end") return 3;
  return 0;  // stretch
}

// -- CSS transform -----------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;

void MatrixIdentity(float* m) {
  for (int i = 0; i < 16; i++) m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b, both column-major 4x4.
void MatrixMultiply(const float* a, const float* b, float* out) {
  float result[16];
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      result[col * 4 + row] = sum;
    }
  }
  std::memcpy(out, result, sizeof(result));
}

bool ParsePxToken(const std::string& raw, double* out) {
  std::string value = TrimAscii(raw);
  if (EndsWith(value, "px")) value.resize(value.size() - 2);
  return ParseNumber(value, out);
}

bool ParseAngleToken(const std::string& raw, double* radians) {
  std::string value = TrimAscii(raw);
  double number = 0.0;
  if (EndsWith(value, "deg")) {
    value.resize(value.size() - 3);
    if (!ParseNumber(value, &number)) return false;
    *radians = number * kPi / 180.0;
    return true;
  }
  if (EndsWith(value, "rad")) {
    value.resize(value.size() - 3);
    if (!ParseNumber(value, &number)) return false;
    *radians = number;
    return true;
  }
  if (EndsWith(value, "turn")) {
    value.resize(value.size() - 4);
    if (!ParseNumber(value, &number)) return false;
    *radians = number * 2.0 * kPi;
    return true;
  }
  if (!ParseNumber(value, &number)) return false;
  *radians = number * kPi / 180.0;  // unitless follows CSS angles
  return true;
}

std::vector<std::string> TransformArguments(const std::string& args) {
  std::vector<std::string> out;
  std::string current;
  for (char c : args) {
    if (c == ',' || c == ' ' || c == '\t') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// Parses `transform: translate(...) rotate(...) scale(...) ...` into a
// column-major matrix.  Percent translations are not supported here (the
// element size is unknown at parse time).
void ParseTransform(const std::string& raw, AppStyle* style) {
  float matrix[16];
  MatrixIdentity(matrix);
  size_t index = 0;
  bool any = false;
  while (index < raw.size()) {
    while (index < raw.size() &&
           (std::isspace(static_cast<unsigned char>(raw[index])) ||
            raw[index] == ',')) {
      index++;
    }
    const size_t name_start = index;
    while (index < raw.size() && raw[index] != '(') index++;
    if (index >= raw.size()) break;
    std::string name = LowerAscii(TrimAscii(raw.substr(name_start,
                                                       index - name_start)));
    const size_t close = raw.find(')', index + 1);
    if (close == std::string::npos) break;
    const std::string args =
        raw.substr(index + 1, close - index - 1);
    index = close + 1;
    const std::vector<std::string> values = TransformArguments(args);
    float local[16];
    MatrixIdentity(local);
    if (name == "translate" || name == "translate3d") {
      double tx = 0.0;
      double ty = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &tx)) continue;
      if (values.size() > 1) ParsePxToken(values[1], &ty);
      double tz = 0.0;
      if (values.size() > 2) ParsePxToken(values[2], &tz);
      local[12] = static_cast<float>(tx);
      local[13] = static_cast<float>(ty);
      local[14] = static_cast<float>(tz);
    } else if (name == "translatex") {
      double tx = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &tx)) continue;
      local[12] = static_cast<float>(tx);
    } else if (name == "translatey") {
      double ty = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &ty)) continue;
      local[13] = static_cast<float>(ty);
    } else if (name == "translatez") {
      double tz = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &tz)) continue;
      local[14] = static_cast<float>(tz);
    } else if (name == "scale" || name == "scale3d") {
      double sx = 1.0;
      double sy = 1.0;
      double sz = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sx)) continue;
      sy = sx;
      if (values.size() > 1) ParseNumber(values[1], &sy);
      if (values.size() > 2) ParseNumber(values[2], &sz);
      local[0] = static_cast<float>(sx);
      local[5] = static_cast<float>(sy);
      local[10] = static_cast<float>(sz);
    } else if (name == "scalex") {
      double sx = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sx)) continue;
      local[0] = static_cast<float>(sx);
    } else if (name == "scaley") {
      double sy = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sy)) continue;
      local[5] = static_cast<float>(sy);
    } else if (name == "scalez") {
      double sz = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sz)) continue;
      local[10] = static_cast<float>(sz);
    } else if (name == "rotate" || name == "rotatez") {
      double radians = 0.0;
      if (values.empty() || !ParseAngleToken(values[0], &radians)) continue;
      const float c = static_cast<float>(std::cos(radians));
      const float s = static_cast<float>(std::sin(radians));
      local[0] = c;
      local[1] = s;
      local[4] = -s;
      local[5] = c;
    } else if (name == "rotatex") {
      double radians = 0.0;
      if (values.empty() || !ParseAngleToken(values[0], &radians)) continue;
      const float c = static_cast<float>(std::cos(radians));
      const float s = static_cast<float>(std::sin(radians));
      // Column-major rotation about X.
      local[5] = c;
      local[6] = s;
      local[9] = -s;
      local[10] = c;
    } else if (name == "rotatey") {
      double radians = 0.0;
      if (values.empty() || !ParseAngleToken(values[0], &radians)) continue;
      const float c = static_cast<float>(std::cos(radians));
      const float s = static_cast<float>(std::sin(radians));
      // Column-major rotation about Y.
      local[0] = c;
      local[2] = -s;
      local[8] = s;
      local[10] = c;
    } else if (name == "skew" || name == "skewx" || name == "skewy") {
      double ax = 0.0;
      double ay = 0.0;
      if (name == "skew") {
        if (values.empty() || !ParseAngleToken(values[0], &ax)) continue;
        if (values.size() > 1) ParseAngleToken(values[1], &ay);
      } else if (name == "skewx") {
        if (values.empty() || !ParseAngleToken(values[0], &ax)) continue;
      } else {
        if (values.empty() || !ParseAngleToken(values[0], &ay)) continue;
      }
      local[1] = static_cast<float>(std::tan(ay));
      local[4] = static_cast<float>(std::tan(ax));
      if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[skyline-runtime] transform %s: ax=%g ay=%g m1=%g m4=%g\n",
                     name.c_str(), ax, ay, local[1], local[4]);
      }
    } else if (name == "perspective") {
      double depth = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &depth) ||
          std::fabs(depth) < 1e-6) {
        continue;
      }
      local[11] = static_cast<float>(-1.0 / depth);
    } else if (name == "matrix") {
      if (values.size() < 6) continue;
      double v[6] = {0, 0, 0, 0, 0, 0};
      bool ok = true;
      for (int i = 0; i < 6; i++) {
        if (!ParseNumber(values[static_cast<size_t>(i)], &v[i])) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      local[0] = static_cast<float>(v[0]);   // a
      local[1] = static_cast<float>(v[1]);   // b
      local[4] = static_cast<float>(v[2]);   // c
      local[5] = static_cast<float>(v[3]);   // d
      local[12] = static_cast<float>(v[4]);  // e
      local[13] = static_cast<float>(v[5]);  // f
    } else if (name == "matrix3d") {
      if (values.size() < 16) continue;
      bool ok = true;
      for (size_t i = 0; i < 16; i++) {
        double value = 0.0;
        if (!ParseNumber(values[i], &value)) {
          ok = false;
          break;
        }
        local[i] = static_cast<float>(value);
      }
      if (!ok) continue;
    } else {
      continue;  // unsupported transform function
    }
    any = true;
    MatrixMultiply(matrix, local, matrix);
  }
  if (!any) return;
  std::memcpy(style->transform, matrix, sizeof(matrix));
  style->has_transform = true;
}

// Splits CSS function arguments at top-level commas (rgba(0, 0, 0, .5) is one
// layer, not four).
std::vector<std::string> SplitTopLevelComma(const std::string& value) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  for (char c : value) {
    if (c == '(') depth++;
    if (c == ')' && depth > 0) depth--;
    if (c == ',' && depth == 0) {
      out.push_back(TrimAscii(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!TrimAscii(current).empty()) out.push_back(TrimAscii(current));
  return out;
}

// CSS time values (`250ms`, `0.3s`).  Bare numbers are treated as ms.
bool ParseTimeMs(const std::string& raw, double* out) {
  std::string value = TrimAscii(raw);
  if (value.empty()) return false;
  double number = 0.0;
  if (EndsWith(value, "ms")) {
    value.resize(value.size() - 2);
    if (!ParseNumber(value, &number)) return false;
    *out = number;
    return true;
  }
  if (EndsWith(value, "s")) {
    value.resize(value.size() - 1);
    if (!ParseNumber(value, &number)) return false;
    *out = number * 1000.0;
    return true;
  }
  if (!ParseNumber(value, &number)) return false;
  *out = number;
  return true;
}

uint32_t TransitionPropertyBit(const std::string& raw) {
  const std::string name = LowerAscii(TrimAscii(raw));
  if (name == "all" || name.empty()) {
    return kTransitionOpacity | kTransitionBackground | kTransitionColor |
           kTransitionBorderColor | kTransitionTransform;
  }
  if (name == "opacity") return kTransitionOpacity;
  if (name == "background" || name == "background-color") {
    return kTransitionBackground;
  }
  if (name == "color") return kTransitionColor;
  if (name == "border" || name == "border-color") {
    return kTransitionBorderColor;
  }
  if (name == "transform") return kTransitionTransform;
  return 0u;
}

int TransitionEasing(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value == "ease") return 1;
  if (value == "ease-in") return 2;
  if (value == "ease-out") return 3;
  if (value == "ease-in-out") return 4;
  if (StartsWith(value, "cubic-bezier")) return 1;  // approximate with ease
  return 0;                                          // linear
}

bool IsTransitionTimingKeyword(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  return value == "linear" || value == "ease" || value == "ease-in" ||
         value == "ease-out" || value == "ease-in-out" ||
         StartsWith(value, "cubic-bezier") || StartsWith(value, "steps");
}

void ParseTransitionShorthand(AppStyle* style, const std::string& raw) {
  const std::vector<std::string> layers = SplitTopLevelComma(raw);
  uint32_t mask = 0;
  double duration = 0.0;
  double delay = 0.0;
  int easing = 0;
  bool have_duration = false;
  bool have_delay = false;
  for (const std::string& layer : layers) {
    // CSS allows one duration/delay/timing per comma-separated transition;
    // reset per layer so a second layer's duration is not mistaken for delay.
    double layer_duration = 0.0;
    double layer_delay = 0.0;
    bool layer_has_duration = false;
    bool layer_has_delay = false;
    int layer_easing = 0;
    uint32_t layer_mask = 0;
    for (const std::string& token : SplitWhitespace(layer)) {
      double time = 0.0;
      if (ParseTimeMs(token, &time)) {
        if (!layer_has_duration) {
          layer_duration = time;
          layer_has_duration = true;
        } else if (!layer_has_delay) {
          layer_delay = time;
          layer_has_delay = true;
        }
      } else if (IsTransitionTimingKeyword(token)) {
        layer_easing = TransitionEasing(token);
      } else {
        layer_mask |= TransitionPropertyBit(token);
      }
    }
    mask |= layer_mask != 0 ? layer_mask : TransitionPropertyBit("all");
    if (!have_duration && layer_has_duration) {
      duration = layer_duration;
      have_duration = true;
    }
    if (!have_delay && layer_has_delay) {
      delay = layer_delay;
      have_delay = true;
    }
    if (easing == 0) easing = layer_easing;
  }
  style->transition_mask = mask != 0 ? mask : TransitionPropertyBit("all");
  style->transition_duration = duration;
  style->transition_delay = delay;
  style->transition_easing = easing;
}

// Extracts `url(...)` from a CSS background value.
std::string ParseCssUrl(const std::string& raw) {
  const size_t start = raw.find("url(");
  if (start == std::string::npos) return std::string();
  size_t value_start = start + 4;
  while (value_start < raw.size() &&
         std::isspace(static_cast<unsigned char>(raw[value_start]))) {
    value_start++;
  }
  const size_t end = raw.find(')', value_start);
  if (end == std::string::npos) return std::string();
  std::string value = TrimAscii(raw.substr(value_start, end - value_start));
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
      value.back() == value.front()) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

int ParseBackgroundRepeat(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value == "repeat" || value == "repeat repeat") return 1;
  if (value == "repeat-x") return 2;
  if (value == "repeat-y") return 3;
  return 0;
}

int ParseBackgroundFit(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value == "cover") return 2;
  if (value == "contain") return 1;
  return 0;  // fill / auto
}

// Parses CSS `transform-origin` (one to three components; z is ignored).
void ParseTransformOrigin(AppStyle* style, const std::string& raw) {
  const std::vector<std::string> parts = SplitWhitespace(raw);
  if (parts.empty()) return;
  bool x_set = false;
  bool y_set = false;
  float x_px = std::nanf("");
  float y_px = std::nanf("");
  float x_percent = std::nanf("");
  float y_percent = std::nanf("");
  auto set_x_percent = [&](float value) {
    x_percent = value;
    x_px = std::nanf("");
    x_set = true;
  };
  auto set_y_percent = [&](float value) {
    y_percent = value;
    y_px = std::nanf("");
    y_set = true;
  };
  for (const std::string& part : parts) {
    const std::string value = LowerAscii(TrimAscii(part));
    if (value == "left") {
      set_x_percent(0.0f);
      continue;
    }
    if (value == "right") {
      set_x_percent(1.0f);
      continue;
    }
    if (value == "top") {
      set_y_percent(0.0f);
      continue;
    }
    if (value == "bottom") {
      set_y_percent(1.0f);
      continue;
    }
    if (value == "center") {
      if (!x_set) {
        set_x_percent(0.5f);
      } else if (!y_set) {
        set_y_percent(0.5f);
      }
      continue;
    }
    float px = 0.0f;
    float percent = -1.0f;
    if (!ParseLength(part, &px, &percent)) continue;
    if (!x_set) {
      if (percent >= 0.0f) {
        set_x_percent(percent);
      } else {
        x_px = px;
        x_percent = std::nanf("");
        x_set = true;
      }
    } else if (!y_set) {
      if (percent >= 0.0f) {
        set_y_percent(percent);
      } else {
        y_px = px;
        y_percent = std::nanf("");
        y_set = true;
      }
    } else {
      break;  // third component is the z offset
    }
  }
  if (x_set && !y_set) set_y_percent(0.5f);
  if (y_set && !x_set) set_x_percent(0.5f);
  style->transform_origin_x = x_px;
  style->transform_origin_y = y_px;
  style->transform_origin_x_percent = x_percent;
  style->transform_origin_y_percent = y_percent;
}

// Parses the first `linear-gradient(...)` layer into AppStyle.
void ParseLinearGradient(AppStyle* style, const std::string& raw) {
  const size_t start = raw.find("linear-gradient(");
  if (start == std::string::npos) return;
  size_t cursor = start + strlen("linear-gradient(");
  int depth = 1;
  const size_t args_start = cursor;
  while (cursor < raw.size() && depth > 0) {
    if (raw[cursor] == '(') depth++;
    if (raw[cursor] == ')') depth--;
    cursor++;
  }
  if (depth != 0) return;
  const std::string args =
      raw.substr(args_start, cursor - args_start - 1);
  const std::vector<std::string> layers = SplitTopLevelComma(args);
  if (layers.empty()) return;

  size_t first_color = 0;
  double angle = 0.0;  // CSS 0deg = to top; NewLinearGradient uses radians.
  {
    const std::string head = LowerAscii(TrimAscii(layers[0]));
    if (StartsWith(head, "to ")) {
      const std::string direction = TrimAscii(head.substr(3));
      if (direction == "right") angle = kPi / 2.0;
      else if (direction == "bottom") angle = kPi;
      else if (direction == "left") angle = -kPi / 2.0;
      else angle = 0.0;  // to top
      first_color = 1;
    } else {
      double parsed = 0.0;
      if (ParseAngleToken(head, &parsed)) {
        angle = parsed;
        first_color = 1;
      }
    }
  }

  std::vector<uint32_t> colors;
  std::vector<double> stops;
  for (size_t i = first_color; i < layers.size(); i++) {
    const std::string part = TrimAscii(layers[i]);
    if (part.empty()) continue;
    std::string color_text;
    std::string stop_text;
    if (StartsWith(LowerAscii(part), "rgb(") ||
        StartsWith(LowerAscii(part), "rgba(")) {
      const size_t close = part.find(')');
      if (close == std::string::npos) continue;
      color_text = part.substr(0, close + 1);
      stop_text = TrimAscii(part.substr(close + 1));
    } else {
      const std::vector<std::string> tokens = SplitWhitespace(part);
      if (tokens.empty()) continue;
      color_text = tokens[0];
      if (tokens.size() > 1) stop_text = tokens[1];
    }
    uint32_t color = 0u;
    if (!ParseCssColor(color_text, &color)) continue;
    double stop = colors.empty() ? 0.0 : -1.0;
    if (!stop_text.empty()) {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(stop_text, &px, &percent)) {
        stop = percent >= 0.0f ? percent : px / 100.0f;
      }
    }
    colors.push_back(color);
    stops.push_back(stop);
  }
  if (colors.size() < 2) return;
  // Distribute unspecified stops evenly.
  double previous = 0.0;
  for (size_t i = 0; i < stops.size(); i++) {
    if (stops[i] < 0.0) {
      stops[i] = static_cast<double>(i) /
                 static_cast<double>(stops.size() - 1);
      if (stops[i] < previous) stops[i] = previous;
    }
    previous = stops[i];
  }
  style->has_gradient = true;
  style->gradient_angle = angle;
  style->gradient_colors = std::move(colors);
  style->gradient_stops = std::move(stops);
}

// Parses CSS `border-radius` (1-4 corner values; elliptical `/` is reduced to
// the horizontal radius).
void ParseBorderRadius(AppStyle* style, const std::string& raw) {
  std::string value = raw;
  const size_t slash = value.find('/');
  if (slash != std::string::npos) value.resize(slash);
  const std::vector<std::string> parts = SplitWhitespace(value);
  if (parts.empty()) return;
  float radii[4] = {0, 0, 0, 0};
  bool percent[4] = {false, false, false, false};
  for (size_t i = 0; i < parts.size() && i < 4; i++) {
    float px = 0.0f;
    float pct = -1.0f;
    if (!ParseLength(parts[i], &px, &pct)) return;
    if (pct >= 0.0f) {
      radii[i] = pct;
      percent[i] = true;
    } else {
      radii[i] = px;
    }
  }
  float tl = radii[0];
  float tr = parts.size() > 1 ? radii[1] : tl;
  float br = parts.size() > 2 ? radii[2] : tl;
  float bl = parts.size() > 3 ? radii[3] : tr;
  if (parts.size() == 1) {
    tr = bl = br = tl;
  }
  const float values[4] = {tl, tr, br, bl};
  const bool flags[4] = {
      percent[0], parts.size() > 1 ? percent[1] : percent[0],
      parts.size() > 2 ? percent[2] : percent[0],
      parts.size() > 3 ? percent[3] : (parts.size() > 1 ? percent[1] : percent[0])};
  for (int i = 0; i < 4; i++) {
    style->corner_radius[i] = values[i];
    style->corner_radius_percent[i] = flags[i];
  }
}

void ParseCornerRadius(AppStyle* style, int corner, const std::string& raw) {
  std::string value = raw;
  const size_t slash = value.find('/');
  if (slash != std::string::npos) value.resize(slash);
  float px = 0.0f;
  float pct = -1.0f;
  if (!ParseLength(TrimAscii(value), &px, &pct)) return;
  if (pct >= 0.0f) {
    style->corner_radius[corner] = pct;
    style->corner_radius_percent[corner] = true;
  } else {
    style->corner_radius[corner] = px;
    style->corner_radius_percent[corner] = false;
  }
}

float EffectiveCornerRadius(const AppStyle& style, int corner) {
  return style.corner_radius[corner] >= 0.0f ? style.corner_radius[corner]
                                             : style.radius;
}

bool CornerRadiusPercent(const AppStyle& style, int corner) {
  return style.corner_radius[corner] >= 0.0f &&
         style.corner_radius_percent[corner];
}

bool HasAnyRadius(const AppStyle& style) {
  if (style.radius > 0.0f) return true;
  for (int i = 0; i < 4; i++) {
    if (EffectiveCornerRadius(style, i) > 0.0f) return true;
  }
  return false;
}

// Border side order is top, right, bottom, left.
float EffectiveBorderWidth(const AppStyle& style, int side) {
  return style.side_border_width[side] >= 0.0f ? style.side_border_width[side]
                                               : style.border_width;
}

uint32_t EffectiveBorderColor(const AppStyle& style, int side) {
  return style.side_border_color_set[side] ? style.side_border_color[side]
                                           : style.border_color;
}

bool HasBorder(const AppStyle& style) {
  for (int i = 0; i < 4; i++) {
    if (EffectiveBorderWidth(style, i) > 0.0f) return true;
  }
  return false;
}

bool BorderSidesChanged(const AppStyle& before, const AppStyle& after) {
  for (int i = 0; i < 4; i++) {
    if (std::fabs(EffectiveBorderWidth(before, i) -
                  EffectiveBorderWidth(after, i)) > 0.01f) {
      return true;
    }
    if (EffectiveBorderColor(before, i) != EffectiveBorderColor(after, i)) {
      return true;
    }
  }
  return false;
}

bool RadiiChanged(const AppStyle& a, const AppStyle& b) {
  auto same = [](float x, float y) {
    if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
    return std::fabs(x - y) < 0.01f;
  };
  if (!same(a.radius, b.radius)) return true;
  for (int i = 0; i < 4; i++) {
    if (!same(a.corner_radius[i], b.corner_radius[i])) return true;
    if (a.corner_radius_percent[i] != b.corner_radius_percent[i]) return true;
  }
  return false;
}

// Resolves transform-origin against the measured border box.
void ResolveTransformOrigin(const AppStyle& style, float width, float height,
                            double* out_x, double* out_y) {
  if (!std::isnan(style.transform_origin_x_percent) &&
      style.transform_origin_x_percent >= 0.0f) {
    *out_x = style.transform_origin_x_percent * width;
  } else if (!std::isnan(style.transform_origin_x)) {
    *out_x = style.transform_origin_x;
  } else {
    *out_x = width * 0.5;
  }
  if (!std::isnan(style.transform_origin_y_percent) &&
      style.transform_origin_y_percent >= 0.0f) {
    *out_y = style.transform_origin_y_percent * height;
  } else if (!std::isnan(style.transform_origin_y)) {
    *out_y = style.transform_origin_y;
  } else {
    *out_y = height * 0.5;
  }
}

// Whitespace tokenizer that keeps `rgba(0, 0, 0, 0.5)` together.
std::vector<std::string> TokenizeCssValue(const std::string& value) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  for (char c : value) {
    if (c == '(') depth++;
    if (c == ')' && depth > 0) depth--;
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// `box-shadow: x y [blur] [spread] color [inset]` (first layer only).
void ParseBoxShadow(const std::string& raw, AppStyle* style) {
  const std::string value = TrimAscii(raw);
  if (value == "none" || value.empty()) {
    style->has_shadow = false;
    return;
  }
  const std::vector<std::string> layers = SplitTopLevelComma(value);
  if (layers.empty()) return;
  const std::vector<std::string> tokens = TokenizeCssValue(layers[0]);
  float numbers[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int number_count = 0;
  bool inset = false;
  bool have_color = false;
  uint32_t color = style->color;
  for (const std::string& token : tokens) {
    const std::string lowered = LowerAscii(token);
    if (lowered == "inset") {
      inset = true;
      continue;
    }
    uint32_t parsed_color = 0u;
    if (ParseCssColor(token, &parsed_color)) {
      color = parsed_color;
      have_color = true;
      continue;
    }
    double number = 0.0;
    if (number_count < 4 && ParsePxToken(token, &number)) {
      numbers[number_count++] = static_cast<float>(number);
    }
  }
  if (number_count < 2) return;
  style->has_shadow = true;
  style->shadow_offset_x = numbers[0];
  style->shadow_offset_y = numbers[1];
  style->shadow_blur = numbers[2];
  style->shadow_spread = numbers[3];
  style->shadow_color = have_color ? color : style->color;
  style->shadow_inset = inset;
}

void ApplyDeclaration(AppStyle* style,
                      const std::string& raw_key,
                      const std::string& raw_value) {
  const std::string key = LowerAscii(TrimAscii(raw_key));
  const std::string value = TrimAscii(raw_value);
  if (key.empty() || value.empty()) return;

  if (key == "display") {
    style->display_none = value == "none";
  } else if (key == "overflow" || key == "overflow-x" ||
             key == "overflow-y") {
    style->clip = value == "hidden" || value == "clip" || value == "scroll" ||
                  value == "auto";
  } else if (key == "position") {
    style->position = value == "fixed" ? 2 : value == "absolute" ? 1 : 0;
  } else if (key == "top" || key == "right" || key == "bottom" ||
             key == "left") {
    int index = key == "top" ? 0 : key == "right" ? 1 : key == "bottom" ? 2 : 3;
    float px = -1.0f;
    float percent = -1.0f;
    if (!ParseLength(value, &px, &percent)) return;
    if (percent >= 0.0f) {
      style->inset[index] = std::nan("");
      style->inset_percent[index] = percent;
    } else if (value == "auto") {
      style->inset[index] = std::nan("");
      style->inset_percent[index] = std::nan("");
    } else {
      style->inset[index] = px;
      style->inset_percent[index] = std::nan("");
    }
  } else if (key == "z-index") {
    double number = 0.0;
    if (ParseNumber(value, &number)) {
      style->z_index = static_cast<int>(number);
    }
  } else if (key == "transform") {
    if (value != "none") ParseTransform(value, style);
  } else if (key == "transform-origin") {
    ParseTransformOrigin(style, value);
  } else if (key == "transition") {
    ParseTransitionShorthand(style, value);
  } else if (key == "transition-property") {
    uint32_t mask = 0;
    for (const std::string& part : SplitTopLevelComma(value)) {
      mask |= TransitionPropertyBit(part);
    }
    style->transition_mask = mask;
  } else if (key == "transition-duration") {
    double duration = 0.0;
    const std::vector<std::string> parts = SplitTopLevelComma(value);
    if (!parts.empty() && ParseTimeMs(parts[0], &duration)) {
      style->transition_duration = duration;
    }
  } else if (key == "transition-delay") {
    double delay = 0.0;
    const std::vector<std::string> parts = SplitTopLevelComma(value);
    if (!parts.empty() && ParseTimeMs(parts[0], &delay)) {
      style->transition_delay = delay;
    }
  } else if (key == "transition-timing-function") {
    style->transition_easing = TransitionEasing(value);
  } else if (key == "box-shadow") {
    ParseBoxShadow(value, style);
  } else if (key == "background-image" || key == "background") {
    if (value.find("linear-gradient") != std::string::npos) {
      ParseLinearGradient(style, value);
    }
    const std::string url = ParseCssUrl(value);
    if (!url.empty()) style->background_image = url;
    if (value.find("repeat-x") != std::string::npos) {
      style->background_repeat = 2;
    } else if (value.find("repeat-y") != std::string::npos) {
      style->background_repeat = 3;
    } else if (value.find("no-repeat") != std::string::npos) {
      style->background_repeat = 0;
    } else if (value.find("repeat") != std::string::npos) {
      style->background_repeat = 1;
    }
    if (value.find("cover") != std::string::npos) style->background_fit = 2;
    else if (value.find("contain") != std::string::npos) style->background_fit = 1;
    if (key == "background") {
      // The shorthand may also carry a colour token.
      for (const std::string& token : TokenizeCssValue(value)) {
        uint32_t color = 0u;
        if (ParseCssColor(token, &color)) {
          style->background = color;
          style->has_background = true;
          break;
        }
      }
    }
  } else if (key == "background-repeat") {
    style->background_repeat = ParseBackgroundRepeat(value);
  } else if (key == "background-size") {
    style->background_fit = ParseBackgroundFit(value);
  } else if (key == "flex-direction") {
    style->row = StartsWith(value, "row");
  } else if (key == "flex-wrap") {
    style->wrap = value == "wrap" || value == "wrap-reverse";
  } else if (key == "align-content") {
    if (value == "flex-start" || value == "start") {
      style->align_content = 1;
    } else if (value == "center") {
      style->align_content = 2;
    } else if (value == "flex-end" || value == "end") {
      style->align_content = 3;
    } else if (value == "space-between" || value == "space-around" ||
               value == "stretch") {
      style->align_content = 0;
    }
  } else if (key == "order") {
    double number = 0.0;
    if (ParseNumber(value, &number)) {
      style->order = static_cast<int>(number);
    }
  } else if (key == "justify-content") {
    style->justify = ParseJustify(value);
  } else if (key == "align-items") {
    style->align = ParseAlign(value);
  } else if (key == "align-self") {
    style->align_self = value == "auto" ? -1 : ParseAlign(value);
  } else if (key == "width" || key == "min-width" || key == "max-width" ||
             key == "height" || key == "min-height" || key == "max-height") {
    float px = -1.0f;
    float percent = -1.0f;
    if (!ParseLength(value, &px, &percent)) return;
    if (key == "width") {
      style->width = px;
      style->width_percent = percent;
    } else if (key == "height") {
      style->height = px;
      style->height_percent = percent;
    } else if (key == "min-width") {
      style->min_width = px > 0 ? px : 0.0f;
    } else if (key == "max-width") {
      style->max_width = px > 0 ? px : kInf;
    } else if (key == "min-height") {
      style->min_height = px > 0 ? px : 0.0f;
    } else if (key == "max-height") {
      style->max_height = px > 0 ? px : kInf;
    }
  } else if (key == "padding" || key == "margin") {
    const std::vector<std::string> parts = SplitWhitespace(value);
    float values[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < parts.size() && i < 4; i++) {
      float px = 0.0f;
      float percent = -1.0f;
      if (!ParseLength(parts[i], &px, &percent)) return;
      // Percentage padding/margin is rare in the recorded pages; use px only.
      values[i] = px > 0 ? px : 0.0f;
    }
    const size_t n = parts.size();
    float top = values[0];
    float right = n > 1 ? values[1] : top;
    float bottom = n > 2 ? values[2] : top;
    float left = n > 3 ? values[3] : right;
    if (n == 2) bottom = top;
    if (n == 3) left = right;
    float* target = style->padding;
    if (key == "margin") target = style->margin;
    target[0] = top;
    target[1] = right;
    target[2] = bottom;
    target[3] = left;
  } else if (key == "padding-top") {
    float dummy = -1.0f;
    ParseLength(value, &style->padding[0], &dummy);
  } else if (key == "padding-right") {
    float dummy = -1.0f;
    ParseLength(value, &style->padding[1], &dummy);
  } else if (key == "padding-bottom") {
    float dummy = -1.0f;
    ParseLength(value, &style->padding[2], &dummy);
  } else if (key == "padding-left") {
    float dummy = -1.0f;
    ParseLength(value, &style->padding[3], &dummy);
  } else if (key == "margin-top") {
    float dummy = -1.0f;
    ParseLength(value, &style->margin[0], &dummy);
  } else if (key == "margin-right") {
    float dummy = -1.0f;
    ParseLength(value, &style->margin[1], &dummy);
  } else if (key == "margin-bottom") {
    float dummy = -1.0f;
    ParseLength(value, &style->margin[2], &dummy);
  } else if (key == "margin-left") {
    float dummy = -1.0f;
    ParseLength(value, &style->margin[3], &dummy);
  } else if (key == "gap" || key == "column-gap" || key == "row-gap") {
    if (key == "gap") {
      const std::vector<std::string> parts = SplitWhitespace(value);
      float row = 0.0f;
      float column = 0.0f;
      float percent = -1.0f;
      if (!parts.empty() && ParseLength(parts[0], &row, &percent)) {
        column = row;
        if (parts.size() > 1) {
          float second_percent = -1.0f;
          ParseLength(parts[1], &column, &second_percent);
        }
        style->gap = row;
        style->row_gap = row;
        style->column_gap = column;
      }
    } else {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(value, &px, &percent) && px >= 0) {
        if (key == "row-gap") {
          style->row_gap = px;
          style->gap = px;
        } else {
          style->column_gap = px;
          style->gap = px;
        }
      }
    }
  } else if (key == "flex") {
    double number = 0.0;
    if (ParseNumber(value, &number)) {
      style->flex = static_cast<float>(number);
      style->has_flex = true;
    }
  } else if (key == "background" || key == "background-color") {
    uint32_t color = 0u;
    if (ParseCssColor(value, &color)) {
      style->background = color;
      style->has_background = true;
    }
  } else if (key == "color") {
    uint32_t color = 0u;
    if (ParseCssColor(value, &color)) style->color = color;
  } else if (key == "font-size") {
    float px = 0.0f;
    float percent = -1.0f;
    if (ParseLength(value, &px, &percent) && px > 0) style->font_size = px;
  } else if (key == "font-weight") {
    if (value == "bold") {
      style->font_weight = 6;
    } else if (value == "normal") {
      style->font_weight = 3;
    } else {
      double number = 0.0;
      if (ParseNumber(value, &number)) {
        int index = static_cast<int>(number / 100.0) - 1;
        if (index < 0) index = 0;
        if (index > 8) index = 8;
        style->font_weight = index;
      }
    }
  } else if (key == "font-style") {
    style->font_style = value == "italic" ? 1 : 0;
  } else if (key == "line-height") {
    if (value == "normal") {
      style->line_height = -1.0;
      style->line_height_factor = 1.2;
    } else if (EndsWith(value, "px")) {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(value, &px, &percent) && px > 0) style->line_height = px;
    } else {
      double number = 0.0;
      if (ParseNumber(value, &number) && number > 0) {
        style->line_height_factor = number;
        style->line_height = -1.0;
      }
    }
  } else if (key == "letter-spacing") {
    float px = 0.0f;
    float percent = -1.0f;
    if (ParseLength(value, &px, &percent)) style->letter_spacing = px;
  } else if (key == "text-align") {
    if (value == "center") style->text_align = 2;
    else if (value == "right" || value == "end") style->text_align = 1;
    else if (value == "justify") style->text_align = 3;
    else style->text_align = 0;
  } else if (key == "border" || key == "border-top" ||
             key == "border-right" || key == "border-bottom" ||
             key == "border-left" || key == "border-top-width" ||
             key == "border-right-width" || key == "border-bottom-width" ||
             key == "border-left-width" || key == "border-top-color" ||
             key == "border-right-color" || key == "border-bottom-color" ||
             key == "border-left-color") {
    const bool uniform = key == "border";
    int side = -1;
    if (!uniform) {
      if (key.find("top") != std::string::npos) side = 0;
      else if (key.find("right") != std::string::npos) side = 1;
      else if (key.find("bottom") != std::string::npos) side = 2;
      else side = 3;
    }
    const bool width_only = EndsWith(key, "-width");
    const bool color_only = EndsWith(key, "-color");
    if (color_only) {
      uint32_t color = 0u;
      if (ParseCssColor(value, &color)) {
        if (uniform) {
          style->border_color = color;
        } else {
          style->side_border_color[side] = color;
          style->side_border_color_set[side] = true;
        }
      }
    } else if (width_only) {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(value, &px, &percent) && px >= 0.0f) {
        if (uniform) {
          style->border_width = px;
        } else {
          style->side_border_width[side] = px;
        }
      }
    } else {
      const std::vector<std::string> parts = SplitWhitespace(value);
      for (const std::string& part : parts) {
        uint32_t color = 0u;
        if (ParseCssColor(part, &color)) {
          if (uniform) {
            style->border_color = color;
          } else {
            style->side_border_color[side] = color;
            style->side_border_color_set[side] = true;
          }
          continue;
        }
        float px = 0.0f;
        float percent = -1.0f;
        if (ParseLength(part, &px, &percent) && px >= 0.0f) {
          if (uniform) {
            style->border_width = px;
          } else {
            style->side_border_width[side] = px;
          }
        }
      }
    }
  } else if (key == "border-width") {
    const std::vector<std::string> parts = SplitWhitespace(value);
    if (!parts.empty()) {
      float values[4] = {0, 0, 0, 0};
      for (size_t i = 0; i < parts.size() && i < 4; i++) {
        float px = 0.0f;
        float percent = -1.0f;
        if (ParseLength(parts[i], &px, &percent) && px >= 0.0f) {
          values[i] = px;
        }
      }
      const size_t n = parts.size();
      float top = values[0];
      float right = n > 1 ? values[1] : top;
      float bottom = n > 2 ? values[2] : top;
      float left = n > 3 ? values[3] : right;
      style->border_width = top;
      style->side_border_width[0] = top;
      style->side_border_width[1] = right;
      style->side_border_width[2] = bottom;
      style->side_border_width[3] = left;
    }
  } else if (key == "border-color") {
    const std::vector<std::string> parts = SplitWhitespace(value);
    if (!parts.empty()) {
      uint32_t values[4] = {0, 0, 0, 0};
      for (size_t i = 0; i < parts.size() && i < 4; i++) {
        uint32_t color = 0u;
        if (ParseCssColor(parts[i], &color)) values[i] = color;
      }
      const size_t n = parts.size();
      uint32_t top = values[0];
      uint32_t right = n > 1 ? values[1] : top;
      uint32_t bottom = n > 2 ? values[2] : top;
      uint32_t left = n > 3 ? values[3] : right;
      style->border_color = top;
      for (int i = 0; i < 4; i++) {
        style->side_border_color[i] = i == 0 ? top : i == 1 ? right : i == 2 ? bottom : left;
        style->side_border_color_set[i] = true;
      }
    }
  } else if (key == "border-radius") {
    ParseBorderRadius(style, value);
  } else if (key == "border-top-left-radius") {
    ParseCornerRadius(style, 0, value);
  } else if (key == "border-top-right-radius") {
    ParseCornerRadius(style, 1, value);
  } else if (key == "border-bottom-right-radius") {
    ParseCornerRadius(style, 2, value);
  } else if (key == "border-bottom-left-radius") {
    ParseCornerRadius(style, 3, value);
  } else if (key == "opacity") {
    double number = 1.0;
    if (ParseNumber(value, &number)) style->opacity = number;
  } else if (key == "object-fit") {
    if (value == "contain") style->object_fit = 1;
    else if (value == "cover") style->object_fit = 2;
    else if (value == "none") style->object_fit = 3;
    else if (value == "scale-down") style->object_fit = 4;
    else style->object_fit = 0;
  } else if (key == "-webkit-line-clamp") {
    double number = 0.0;
    if (ParseNumber(value, &number) && number > 0) {
      style->max_lines = static_cast<uint32_t>(number);
    }
  }
}

void ApplyDeclarations(AppStyle* style,
                       const std::vector<std::pair<std::string, std::string>>& decls) {
  for (const auto& decl : decls) ApplyDeclaration(style, decl.first, decl.second);
}

std::vector<std::pair<std::string, std::string>> ParseDeclarations(
    const std::string& body) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const std::string& raw : SplitChar(body, ';')) {
    const size_t colon = raw.find(':');
    if (colon == std::string::npos) continue;
    out.emplace_back(TrimAscii(raw.substr(0, colon)),
                     TrimAscii(raw.substr(colon + 1)));
  }
  return out;
}

// -- CSS ---------------------------------------------------------------------

struct Selector {
  std::string element;
  std::vector<std::string> classes;
  std::string id;
};

Selector ParseSelector(const std::string& raw) {
  Selector selector;
  std::string token;
  auto flush = [&]() {
    if (token.empty()) return;
    if (token[0] == '.') {
      selector.classes.push_back(token.substr(1));
    } else if (token[0] == '#') {
      selector.id = token.substr(1);
    } else if (token != "*") {
      selector.element = token;
    }
    token.clear();
  };
  // Descendant combinators are not needed by the recorded pages; matching the
  // last compound selector is a reasonable approximation.
  std::string last;
  std::vector<std::string> compounds = SplitWhitespace(raw);
  last = compounds.empty() ? raw : compounds.back();
  for (char c : last) {
    if (c == '.' || c == '#' || c == ':' || c == '*' || c == '[' || c == ']') {
      flush();
      if (c == '.') token = ".";
      else if (c == '#') token = "#";
      else {
        // Pseudo-classes and attribute selectors are ignored.
        token.clear();
      }
    } else {
      token.push_back(c);
    }
  }
  flush();
  return selector;
}

int SelectorSpecificity(const Selector& selector) {
  return static_cast<int>(selector.classes.size()) * 10 +
         (selector.element.empty() ? 0 : 1) + (selector.id.empty() ? 0 : 100);
}

bool NodeHasClass(const AppNode* node, const std::string& name) {
  for (const std::string& klass : node->classes) {
    if (klass == name) return true;
  }
  return false;
}

bool SelectorMatches(const Selector& selector, const AppNode* node) {
  if (!selector.element.empty() && selector.element != node->tag &&
      selector.element != node->name) {
    return false;
  }
  if (!selector.id.empty()) {
    const auto found = node->attrs.find("id");
    if (found == node->attrs.end() || found->second != selector.id) return false;
  }
  for (const std::string& klass : selector.classes) {
    if (!NodeHasClass(node, klass)) return false;
  }
  return true;
}

void ParseStyleSheet(const std::string& css) {
  std::string cleaned;
  cleaned.reserve(css.size());
  for (size_t i = 0; i < css.size();) {
    if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      const size_t end = css.find("*/", i + 2);
      i = end == std::string::npos ? css.size() : end + 2;
      continue;
    }
    cleaned.push_back(css[i++]);
  }

  size_t cursor = 0;
  while (cursor < cleaned.size()) {
    const size_t open = cleaned.find('{', cursor);
    if (open == std::string::npos) break;
    const size_t close = cleaned.find('}', open + 1);
    if (close == std::string::npos) break;
    const std::string selectors = TrimAscii(cleaned.substr(cursor, open - cursor));
    const std::string body = cleaned.substr(open + 1, close - open - 1);
    const auto declarations = ParseDeclarations(body);
    if (selectors == "@font-face") {
      FontFaceRequest font;
      for (const auto& decl : declarations) {
        const std::string key = LowerAscii(TrimAscii(decl.first));
        if (key == "font-family") {
          std::string family = TrimAscii(decl.second);
          if (family.size() >= 2 &&
              (family.front() == '"' || family.front() == '\'') &&
              family.back() == family.front()) {
            family = family.substr(1, family.size() - 2);
          }
          font.family = family;
        } else if (key == "src") {
          const size_t url_at = decl.second.find("url(");
          if (url_at != std::string::npos) {
            const size_t begin = url_at + 4;
            const size_t end = decl.second.find(')', begin);
            if (end != std::string::npos) {
              std::string url =
                  TrimAscii(decl.second.substr(begin, end - begin));
              if (url.size() >= 2 &&
                  (url.front() == '"' || url.front() == '\'') &&
                  url.back() == url.front()) {
                url = url.substr(1, url.size() - 2);
              }
              font.url = url;
            }
          }
        }
      }
      if (!font.family.empty() && !font.url.empty() && g_page != nullptr) {
        g_page->fonts.push_back(font);
      }
    } else {
      for (const std::string& selector_text : SplitChar(selectors, ',')) {
        if (selector_text.empty()) continue;
        CssRule rule;
        rule.selector = selector_text;
        rule.declarations = declarations;
        rule.order = g_next_rule_order++;
        if (g_page != nullptr) g_page->rules.push_back(rule);
      }
    }
    cursor = close + 1;
  }
}

void ResolveStyles(AppNode* node, const AppStyle* parent) {
  AppStyle style;
  if (parent != nullptr) {
    // CSS inheritance for text properties.
    style.font_size = parent->font_size;
    style.color = parent->color;
    style.font_weight = parent->font_weight;
    style.font_style = parent->font_style;
    style.line_height = parent->line_height;
    style.line_height_factor = parent->line_height_factor;
    style.letter_spacing = parent->letter_spacing;
    style.text_align = parent->text_align;
  }

  if (g_page != nullptr) {
    struct Match {
      int specificity;
      int order;
      const CssRule* rule;
    };
    std::vector<Match> matches;
    for (const CssRule& rule : g_page->rules) {
      const Selector selector = ParseSelector(rule.selector);
      if (SelectorMatches(selector, node)) {
        matches.push_back(
            Match{SelectorSpecificity(selector), rule.order, &rule});
      }
    }
    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                if (a.specificity != b.specificity) {
                  return a.specificity < b.specificity;
                }
                return a.order < b.order;
              });
    for (const Match& match : matches) {
      ApplyDeclarations(&style, match.rule->declarations);
    }
  }
  if (!node->inline_style.empty()) {
    ApplyDeclarations(&style, ParseDeclarations(node->inline_style));
  }
  node->style = style;
  // A running CSS transition writes an interpolated style over the cascade
  // result.  Children inherit from the animated style so text colour/opacity
  // transitions stay in sync with their container.
  if (node->animation_override_active) {
    node->style = node->animation_override;
  }
  for (AppNode* child : node->children) ResolveStyles(child, &node->style);
}

// -- node table / operations -------------------------------------------------

AppNode* NewNode(uint32_t id, const std::string& tag, const std::string& name) {
  AppNode* node = new AppNode();
  node->id = id;
  node->tag = tag;
  node->name = name;
  node->address = g_next_address;
  g_next_address += 0x100;
  if (g_page != nullptr) g_page->nodes[id] = node;
  return node;
}

AppNode* FindNode(uint32_t id) {
  if (g_page == nullptr) return nullptr;
  const auto found = g_page->nodes.find(id);
  return found == g_page->nodes.end() ? nullptr : found->second;
}

void DetachFromParent(AppNode* node) {
  if (node == nullptr || node->parent == nullptr) return;
  auto& siblings = node->parent->children;
  siblings.erase(std::remove(siblings.begin(), siblings.end(), node),
                 siblings.end());
  node->parent = nullptr;
}

void AppendChild(AppNode* parent, AppNode* child) {
  if (parent == nullptr || child == nullptr) return;
  DetachFromParent(child);
  parent->children.push_back(child);
  child->parent = parent;
}

void InsertBefore(AppNode* parent, AppNode* child, AppNode* before) {
  if (parent == nullptr || child == nullptr) return;
  DetachFromParent(child);
  auto& siblings = parent->children;
  const auto found = std::find(siblings.begin(), siblings.end(), before);
  const auto position = found == siblings.end() ? siblings.end() : found;
  siblings.insert(position, child);
  child->parent = parent;
}

void RemoveChild(AppNode* parent, AppNode* child) {
  if (parent == nullptr || child == nullptr) return;
  if (child->parent != parent) return;
  DetachFromParent(child);
}

void SetClasses(AppNode* node, const std::vector<std::string>& classes) {
  node->classes = classes;
}

std::vector<std::string> SplitClasses(const std::string& names) {
  std::vector<std::string> classes;
  for (const std::string& name : SplitWhitespace(names)) {
    if (!name.empty()) classes.push_back(name);
  }
  return classes;
}

void Splice(AppNode* parent, int position, int count,
            const std::vector<AppNode*>& items) {
  if (parent == nullptr) return;
  auto& children = parent->children;
  if (position < 0 || position > static_cast<int>(children.size())) {
    position = static_cast<int>(children.size());
  }
  if (count < 0) count = 0;
  if (position + count > static_cast<int>(children.size())) {
    count = static_cast<int>(children.size()) - position;
  }
  for (int i = 0; i < count; i++) {
    AppNode* removed = children[position + i];
    removed->parent = nullptr;
  }
  children.erase(children.begin() + position, children.begin() + position + count);
  int insert_at = position;
  for (AppNode* item : items) {
    if (item == nullptr) continue;
    DetachFromParent(item);
    if (item->tag == "fragment") {
      std::vector<AppNode*> nested = item->children;
      item->children.clear();
      for (AppNode* child : nested) {
        child->parent = parent;
        children.insert(children.begin() + insert_at++, child);
      }
      continue;
    }
    item->parent = parent;
    children.insert(children.begin() + insert_at++, item);
  }
}

void CancelPendingImage(AppNode* node);

void ApplyAttribute(AppNode* node, const std::string& name,
                    const std::string& value) {
  if (name == "class") {
    SetClasses(node, SplitClasses(value));
  } else if (name == "style") {
    if (!node->inline_style.empty() && node->inline_style.back() != ';') {
      node->inline_style.push_back(';');
    }
    node->inline_style += value;
  } else if (name == "src" || name == "url" || name == "imageUrl" ||
             name == "image" || name == "path") {
    const auto previous = node->attrs.find(name);
    if (previous == node->attrs.end() || previous->second != value) {
      CancelPendingImage(node);
    }
    node->attrs[name] = value;
  } else {
    node->attrs[name] = value;
  }
}

// -- measurement / layout ----------------------------------------------------

struct SizeF {
  float w = 0.0f;
  float h = 0.0f;
};

const char* NodeText(const AppNode* node, std::string* storage) {
  if (!node->text.empty()) return node->text.c_str();
  storage->clear();
  for (const AppNode* child : node->children) {
    if (child->released || child->style.display_none) continue;
    std::string nested;
    storage->append(NodeText(child, &nested));
  }
  return storage->c_str();
}

double GlyphWidth(uint32_t codepoint, double font_size) {
  if (codepoint == ' ') return 0.30 * font_size;
  if (codepoint == '\t') return 1.20 * font_size;
  if (codepoint < 0x20) return 0.0;
  if (codepoint < 0x80) {
    if ((codepoint >= '0' && codepoint <= '9') ||
        (codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= 'a' && codepoint <= 'z')) {
      return 0.56 * font_size;
    }
    if (codepoint == '.' || codepoint == ',' || codepoint == ':' ||
        codepoint == '\'' || codepoint == ';') {
      return 0.28 * font_size;
    }
    return 0.50 * font_size;
  }
  return font_size;  // CJK / full width
}

// Decodes UTF-8 lazily; invalid bytes are treated as single-byte glyphs.
uint32_t NextCodepoint(const std::string& text, size_t* index) {
  const unsigned char c = static_cast<unsigned char>(text[*index]);
  if (c < 0x80) {
    (*index)++;
    return c;
  }
  int extra = 0;
  uint32_t value = 0;
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    value = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    value = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    value = c & 0x07;
  } else {
    (*index)++;
    return c;
  }
  (*index)++;
  for (int i = 0; i < extra && *index < text.size(); i++) {
    const unsigned char next = static_cast<unsigned char>(text[(*index)++]);
    if ((next & 0xC0) != 0x80) return '?';
    value = (value << 6) | (next & 0x3F);
  }
  return value;
}

double TextLineHeight(const AppStyle& style) {
  if (style.line_height > 0) return style.line_height;
  return style.font_size * style.line_height_factor;
}

double TextIntrinsicWidth(const std::string& text, const AppStyle& style) {
  size_t index = 0;
  double width = 0.0;
  while (index < text.size()) {
    const uint32_t codepoint = NextCodepoint(text, &index);
    if (codepoint == '\n') continue;
    width += GlyphWidth(codepoint, style.font_size);
    if (!std::isnan(style.letter_spacing)) width += style.letter_spacing;
  }
  return width;
}

int CountTextLines(const std::string& text, double max_width,
                   const AppStyle& style) {
  if (max_width <= 0.0 || max_width >= kInf) {
    int lines = 1;
    for (char c : text) {
      if (c == '\n') lines++;
    }
    return lines;
  }
  int lines = 1;
  double line_width = 0.0;
  size_t index = 0;
  while (index < text.size()) {
    const uint32_t codepoint = NextCodepoint(text, &index);
    if (codepoint == '\n') {
      lines++;
      line_width = 0.0;
      continue;
    }
    const double glyph = GlyphWidth(codepoint, style.font_size);
    if (line_width > 0.0 && line_width + glyph > max_width + 0.01) {
      lines++;
      line_width = 0.0;
    }
    line_width += glyph;
    if (!std::isnan(style.letter_spacing)) line_width += style.letter_spacing;
  }
  return lines;
}

bool HasExplicitWidth(const AppStyle& style) {
  return style.width >= 0.0f || style.width_percent >= 0.0f;
}

bool HasExplicitHeight(const AppStyle& style) {
  return style.height >= 0.0f || style.height_percent >= 0.0f;
}

// Main-axis gap: row flex uses column-gap, column flex uses row-gap.
float MainGap(const AppStyle& style) {
  if (style.row) {
    return style.column_gap > 0.0f ? style.column_gap : style.gap;
  }
  return style.row_gap > 0.0f ? style.row_gap : style.gap;
}

int EffectiveAlign(const AppStyle& parent, const AppStyle& child) {
  return child.align_self >= 0 ? child.align_self : parent.align;
}

// Flow children in CSS `order` order (stable for equal order values).
std::vector<AppNode*> OrderedFlowChildren(AppNode* node) {
  std::vector<AppNode*> flow;
  for (AppNode* child : node->children) {
    if (child == nullptr || child->released || child->style.display_none ||
        child->style.position != 0) {
      continue;
    }
    flow.push_back(child);
  }
  std::stable_sort(flow.begin(), flow.end(),
                   [](const AppNode* a, const AppNode* b) {
                     return a->style.order < b->style.order;
                   });
  return flow;
}

// Resolves a CSS inset (top/right/bottom/left) against the parent content box.
// Returns false when the inset is `auto`.
bool ResolveInset(const AppStyle& style, int index, float content_extent,
                  float* out) {
  const float percent = style.inset_percent[index];
  if (!std::isnan(percent) && percent >= 0.0f) {
    *out = content_extent * percent;
    return true;
  }
  const float px = style.inset[index];
  if (!std::isnan(px)) {
    *out = px;
    return true;
  }
  return false;
}

// Forward declarations: image/builder helpers live with the widget
// materialization code, but measurement needs them.
bool IsImageName(const std::string& name);
bool IsListBuilderName(const std::string& name);
bool IsGridBuilderName(const std::string& name);
bool LoadImageForNode(AppNode* node);
bool LoadBackgroundImageForNode(AppNode* node);
void CancelPendingImage(AppNode* node);
void ClearTransitions();

bool ImageUsesWidthFix(const AppNode* node) {
  static const char* kKeys[] = {"mode", "objectFit", "object-fit"};
  for (const char* key : kKeys) {
    const auto found = node->attrs.find(key);
    if (found != node->attrs.end() && found->second == "widthFix") return true;
  }
  return false;
}

float BorderBoxExtra(const AppStyle& style, bool horizontal) {
  const float pad = horizontal ? style.padding[1] + style.padding[3]
                               : style.padding[0] + style.padding[2];
  const float border =
      horizontal ? EffectiveBorderWidth(style, 1) + EffectiveBorderWidth(style, 3)
                 : EffectiveBorderWidth(style, 0) + EffectiveBorderWidth(style, 2);
  return pad + border;
}

SizeF MeasureNode(AppNode* node, float avail_w, bool stretch_w, float avail_h,
                  bool stretch_h) {
  const AppStyle& style = node->style;
  SizeF size{0.0f, 0.0f};
  if (node->released || style.display_none) {
    node->measured_w = 0.0f;
    node->measured_h = 0.0f;
    return size;
  }

  const bool is_text = node->tag == "text";
  std::string text_storage;
  const std::string text =
      is_text ? std::string(NodeText(node, &text_storage)) : std::string();

  float width = -1.0f;
  if (style.width_percent >= 0.0f) {
    width = avail_w * style.width_percent;
  } else if (style.width >= 0.0f) {
    width = style.width;
  } else if (is_text) {
    width = static_cast<float>(TextIntrinsicWidth(text, style)) +
            BorderBoxExtra(style, true);
  } else if (stretch_w) {
    width = avail_w;
  }
  if (width >= 0.0f) {
    if (width < style.min_width) width = style.min_width;
    if (width > style.max_width) width = style.max_width;
  }

  // List/grid builders are slivers: Flutter positions their items, so measure
  // the builder box from its declared child count instead of recursing into
  // the (possibly huge) captured item table.
  if (IsListBuilderName(node->name) || IsGridBuilderName(node->name)) {
    float height = style.height >= 0.0f ? style.height : -1.0f;
    if (height < 0.0f && style.height_percent >= 0.0f) {
      height = avail_h * style.height_percent;
    }
    if (height < 0.0f) {
      int child_count = 0;
      double child_height = 0.0;
      const auto count_attr = node->attrs.find("childCount");
      if (count_attr != node->attrs.end()) {
        child_count = std::atoi(count_attr->second.c_str());
      }
      const auto height_attr = node->attrs.find("childHeight");
      if (height_attr != node->attrs.end()) {
        child_height = std::atof(height_attr->second.c_str());
      }
      if (IsGridBuilderName(node->name)) {
        double cross = 1.0;
        const auto cross_attr = node->attrs.find("crossAxisCount");
        if (cross_attr != node->attrs.end()) {
          cross = std::atof(cross_attr->second.c_str());
        }
        const int columns = static_cast<int>(cross > 0 ? cross : 1);
        const int rows = (child_count + columns - 1) / columns;
        height = static_cast<float>(rows * child_height);
      } else {
        height = static_cast<float>(child_count * child_height);
      }
    }
    if (width < 0.0f) width = stretch_w ? avail_w : 0.0f;
    node->measured_w = width;
    node->measured_h = height;
    return SizeF{width, height};
  }

  // Images: `widthFix` preserves the intrinsic aspect ratio, so the height is
  // derived from the decoded bitmap rather than the CSS box.
  if (IsImageName(node->name)) {
    LoadImageForNode(node);
    if (width >= 0.0f && ImageUsesWidthFix(node) && node->image_dl != nullptr &&
        node->image_dl->width() > 0) {
      const double ratio = static_cast<double>(node->image_dl->height()) /
                           static_cast<double>(node->image_dl->width());
      float height = static_cast<float>(width * ratio) +
                     BorderBoxExtra(style, false);
      if (height < style.min_height) height = style.min_height;
      if (height > style.max_height) height = style.max_height;
      node->measured_w = width;
      node->measured_h = height;
      return SizeF{width, height};
    }
  }

  if (is_text) {
    const float content_w = width >= 0.0f ? width - BorderBoxExtra(style, true)
                                          : static_cast<float>(TextIntrinsicWidth(text, style));
    const int lines = CountTextLines(text, content_w, style);
    float height = style.height >= 0.0f
                       ? style.height
                       : static_cast<float>(lines * TextLineHeight(style)) +
                             BorderBoxExtra(style, false);
    if (height < style.min_height) height = style.min_height;
    if (height > style.max_height) height = style.max_height;
    if (width < 0.0f) width = content_w + BorderBoxExtra(style, true);
    node->measured_w = width;
    node->measured_h = height;
    return SizeF{width, height};
  }

  const float content_avail_w =
      width >= 0.0f ? std::max(0.0f, width - BorderBoxExtra(style, true))
                    : avail_w;

  auto measure_children = [&]() {
    float main_extent = 0.0f;
    float cross_extent = 0.0f;
    int flow_children = 0;
    const float main_gap = MainGap(style);
    const float cross_gap = style.row ? style.row_gap : style.column_gap;
    const float explicit_height =
        style.height >= 0.0f
            ? style.height
            : (style.height_percent >= 0.0f
                   ? avail_h * style.height_percent
                   : -1.0f);
    const float wrap_limit =
        style.wrap
            ? (style.row
                   ? content_avail_w
                   : (explicit_height >= 0.0f
                          ? explicit_height - BorderBoxExtra(style, false)
                          : kInf))
            : kInf;
    float line_main = 0.0f;
    float line_cross = 0.0f;
    float max_line_main = 0.0f;
    float total_cross = 0.0f;
    int lines = 0;
    auto finish_line = [&]() {
      if (line_main <= 0.0f && line_cross <= 0.0f) return;
      max_line_main = std::max(max_line_main, line_main);
      total_cross += line_cross;
      lines++;
      line_main = 0.0f;
      line_cross = 0.0f;
    };
    for (AppNode* child : node->children) {
      if (child != nullptr &&
          (child->released || child->style.display_none)) {
        child->measured_w = 0.0f;
        child->measured_h = 0.0f;
      }
    }
    for (AppNode* child : OrderedFlowChildren(node)) {
      ++flow_children;
      const int child_align = EffectiveAlign(style, child->style);
      const bool child_stretch_w =
          !style.wrap && !style.row && child_align == 0 &&
          !HasExplicitWidth(child->style);
      const bool child_stretch_h =
          !style.wrap && style.row && child_align == 0 &&
          !HasExplicitHeight(child->style);
      const SizeF measured = MeasureNode(child, content_avail_w, child_stretch_w,
                                         kInf, child_stretch_h);
      const float outer_w = measured.w + child->style.margin[1] +
                            child->style.margin[3];
      const float outer_h = measured.h + child->style.margin[0] +
                            child->style.margin[2];
      if (style.wrap && wrap_limit < kInf) {
        const float outer_main = style.row ? outer_w : outer_h;
        const float outer_cross = style.row ? outer_h : outer_w;
        if (line_main > 0.0f &&
            line_main + main_gap + outer_main > wrap_limit + 0.01f) {
          finish_line();
        }
        if (line_main > 0.0f) line_main += main_gap;
        line_main += outer_main;
        line_cross = std::max(line_cross, outer_cross);
      } else if (style.row) {
        main_extent += outer_w;
        cross_extent = std::max(cross_extent, outer_h);
      } else {
        main_extent += outer_h;
        cross_extent = std::max(cross_extent, outer_w);
      }
    }
    if (style.wrap && wrap_limit < kInf) {
      finish_line();
      if (lines > 1) {
        total_cross += cross_gap * static_cast<float>(lines - 1);
      }
      return style.row ? SizeF{max_line_main, total_cross}
                       : SizeF{total_cross, max_line_main};
    }
    const float gaps =
        flow_children > 1
            ? main_gap * static_cast<float>(flow_children - 1)
            : 0.0f;
    main_extent += gaps;
    return style.row ? SizeF{main_extent, cross_extent}
                     : SizeF{cross_extent, main_extent};
  };

  SizeF content = measure_children();
  const float extra_w = BorderBoxExtra(style, true);
  const float extra_h = BorderBoxExtra(style, false);
  if (width < 0.0f) {
    width = content.w + extra_w;
    if (width < style.min_width) width = style.min_width;
    if (width > style.max_width) width = style.max_width;
  }
  float height = -1.0f;
  if (style.height_percent >= 0.0f) {
    height = avail_h * style.height_percent;
  } else if (style.height >= 0.0f) {
    height = style.height;
  } else if (stretch_h) {
    height = avail_h;
  }
  if (height < 0.0f) {
    SizeF remeasured = measure_children();
    height = remeasured.h + extra_h;
    if (height < style.min_height) height = style.min_height;
    if (height > style.max_height) height = style.max_height;
  }

  node->measured_w = width;
  node->measured_h = height;
  return SizeF{width, height};
}

struct NodeMetric {
  AppNode* node = nullptr;
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

// Collects the final rects instead of writing through cached typed-data
// pointers: Dart's GC may move typed data between API calls, so each buffer is
// acquired only for the duration of its own write.
//
// `abs_x`/`abs_y` are the node's border-box origin relative to the viewport.
// They let `position: fixed` resolve against the viewport instead of the
// parent's content box.
void LayoutNode(AppNode* node, float x, float y, float w, float h,
                std::vector<NodeMetric>* metrics, float abs_x = 0.0f,
                float abs_y = 0.0f) {
  node->abs_x = abs_x;
  node->abs_y = abs_y;
  if (metrics != nullptr) {
    metrics->push_back(NodeMetric{node, x, y, w, h});
  }

  const AppStyle& style = node->style;
  if (node->children.empty()) return;
  // Builders materialize items on demand; the Flutter sliver owns placement.
  if (IsListBuilderName(node->name) || IsGridBuilderName(node->name)) return;

  const float content_x =
      EffectiveBorderWidth(style, 3) + style.padding[3];
  const float content_y =
      EffectiveBorderWidth(style, 0) + style.padding[0];
  const float content_w =
      std::max(0.0f, w - BorderBoxExtra(style, true));
  const float content_h =
      std::max(0.0f, h - BorderBoxExtra(style, false));

  struct ChildLayout {
    AppNode* node = nullptr;
    float w = 0.0f;
    float h = 0.0f;
    float margin_main_start = 0.0f;
    float margin_cross_start = 0.0f;
    float margin_main_end = 0.0f;
    float margin_cross_end = 0.0f;
    float main = 0.0f;
    float cross = 0.0f;
  };
  std::vector<ChildLayout> children;
  children.reserve(node->children.size());

  float fixed_sum = 0.0f;
  float flex_sum = 0.0f;
  for (AppNode* child : OrderedFlowChildren(node)) {
    ChildLayout layout;
    layout.node = child;
    // Re-measure with the final parent content box so percentages resolve.
    const bool stretch_cross =
        !style.wrap && EffectiveAlign(style, child->style) == 0 &&
        (style.row ? !HasExplicitHeight(child->style)
                   : !HasExplicitWidth(child->style));
    SizeF measured =
        MeasureNode(child, content_w, !style.row && stretch_cross, content_h,
                    style.row && stretch_cross);
    layout.w = measured.w;
    layout.h = measured.h;
    if (style.row) {
      layout.w = std::max(layout.w, child->style.min_width);
      if (stretch_cross) layout.h = std::max(0.0f, content_h);
      layout.margin_main_start = child->style.margin[3];
      layout.margin_main_end = child->style.margin[1];
      layout.margin_cross_start = child->style.margin[0];
      layout.margin_cross_end = child->style.margin[2];
      layout.main = layout.w;
      layout.cross = layout.h;
    } else {
      if (stretch_cross) layout.w = std::max(0.0f, content_w);
      layout.margin_main_start = child->style.margin[0];
      layout.margin_main_end = child->style.margin[2];
      layout.margin_cross_start = child->style.margin[3];
      layout.margin_cross_end = child->style.margin[1];
      layout.main = layout.h;
      layout.cross = layout.w;
    }
    if (child->style.has_flex && child->style.flex > 0.0f) {
      flex_sum += child->style.flex;
    } else {
      fixed_sum += layout.main + layout.margin_main_start +
                   layout.margin_main_end;
    }
    children.push_back(layout);
  }

  if (style.wrap) {
    // Wrapped flex: break children into lines along the main axis, distribute
    // flex growth per line, then align lines with align-content.
    const float main_gap = MainGap(style);
    const float cross_gap = style.row ? style.row_gap : style.column_gap;
    const float main_extent = style.row ? content_w : content_h;
    const float cross_extent = style.row ? content_h : content_w;
    struct WrapLine {
      std::vector<size_t> indices;
      float main = 0.0f;
      float cross = 0.0f;
      float flex_sum = 0.0f;
    };
    std::vector<WrapLine> lines;
    for (size_t i = 0; i < children.size(); i++) {
      ChildLayout& child = children[i];
      const float outer_main =
          child.main + child.margin_main_start + child.margin_main_end;
      if (lines.empty()) lines.push_back(WrapLine());
      WrapLine* line = &lines.back();
      const float needed =
          line->indices.empty() ? outer_main : line->main + main_gap + outer_main;
      if (!line->indices.empty() && needed > main_extent + 0.01f) {
        lines.push_back(WrapLine());
        line = &lines.back();
      }
      if (!line->indices.empty()) line->main += main_gap;
      line->indices.push_back(i);
      line->main += outer_main;
      line->cross = std::max(
          line->cross,
          child.cross + child.margin_cross_start + child.margin_cross_end);
      if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
        line->flex_sum += child.node->style.flex;
      }
    }
    for (WrapLine& line : lines) {
      if (line.flex_sum <= 0.0f) continue;
      float remaining = main_extent - line.main;
      if (remaining < 0.0f) remaining = 0.0f;
      for (size_t index : line.indices) {
        ChildLayout& child = children[index];
        if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
          child.main +=
              remaining * child.node->style.flex / line.flex_sum;
          if (style.row) {
            child.w = child.main;
          } else {
            child.h = child.main;
          }
        }
      }
    }
    float total_cross =
        cross_gap * (lines.size() > 1 ? static_cast<float>(lines.size() - 1)
                                      : 0.0f);
    for (const WrapLine& line : lines) total_cross += line.cross;
    float cross_offset = 0.0f;
    if (style.align_content == 2) {
      cross_offset = (cross_extent - total_cross) / 2.0f;
    } else if (style.align_content == 3) {
      cross_offset = cross_extent - total_cross;
    } else if (style.align_content == 0 && !lines.empty() &&
               total_cross < cross_extent) {
      const float extra = (cross_extent - total_cross) / lines.size();
      for (WrapLine& line : lines) line.cross += extra;
    }
    float line_cross_pos = (style.row ? content_y : content_x) + cross_offset;
    for (WrapLine& line : lines) {
      float used_main = main_gap * (line.indices.size() > 1
                                        ? static_cast<float>(line.indices.size() - 1)
                                        : 0.0f);
      for (size_t index : line.indices) {
        const ChildLayout& child = children[index];
        used_main += child.main + child.margin_main_start +
                     child.margin_main_end;
      }
      float leftover = main_extent - used_main;
      if (leftover < 0.0f) leftover = 0.0f;
      float justify_offset = 0.0f;
      float extra_gap = 0.0f;
      if (style.justify == 1) {
        justify_offset = leftover / 2.0f;
      } else if (style.justify == 2) {
        justify_offset = leftover;
      } else if (style.justify == 3 && line.indices.size() > 1) {
        extra_gap = leftover / static_cast<float>(line.indices.size() - 1);
      }
      float cursor = (style.row ? content_x : content_y) + justify_offset;
      for (size_t index : line.indices) {
        ChildLayout& child = children[index];
        const int align = EffectiveAlign(style, child.node->style);
        if (align == 0) {
          if (style.row && !HasExplicitHeight(child.node->style)) {
            child.cross = std::max(0.0f, line.cross -
                                             child.margin_cross_start -
                                             child.margin_cross_end);
            child.h = child.cross;
          } else if (!style.row && !HasExplicitWidth(child.node->style)) {
            child.cross = std::max(0.0f, line.cross -
                                             child.margin_cross_start -
                                             child.margin_cross_end);
            child.w = child.cross;
          }
        }
        float child_x = 0.0f;
        float child_y = 0.0f;
        if (style.row) {
          child_y = line_cross_pos;
          const float cross_space = line.cross - child.cross -
                                    child.margin_cross_start -
                                    child.margin_cross_end;
          if (align == 2) {
            child_y += child.margin_cross_start + cross_space / 2.0f;
          } else if (align == 3) {
            child_y += child.margin_cross_start + cross_space;
          } else {
            child_y += child.margin_cross_start;
          }
          cursor += child.margin_main_start;
          child_x = cursor;
          cursor += child.main + child.margin_main_end + main_gap + extra_gap;
        } else {
          child_x = line_cross_pos;
          const float cross_space = line.cross - child.cross -
                                    child.margin_cross_start -
                                    child.margin_cross_end;
          if (align == 2) {
            child_x += child.margin_cross_start + cross_space / 2.0f;
          } else if (align == 3) {
            child_x += child.margin_cross_start + cross_space;
          } else {
            child_x += child.margin_cross_start;
          }
          cursor += child.margin_main_start;
          child_y = cursor;
          cursor += child.main + child.margin_main_end + main_gap + extra_gap;
        }
        LayoutNode(child.node, child_x, child_y, child.w, child.h, metrics,
                   abs_x + child_x, abs_y + child_y);
      }
      line_cross_pos += line.cross + cross_gap;
    }
  } else {
  const float gaps =
      children.size() > 1
          ? MainGap(style) * static_cast<float>(children.size() - 1)
          : 0.0f;
  const float main_extent = style.row ? content_w : content_h;
  float remaining = main_extent - fixed_sum - gaps;
  if (remaining < 0.0f) remaining = 0.0f;
  float flex_extra_total = 0.0f;
  if (flex_sum > 0.0f) {
    for (ChildLayout& child : children) {
      if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
        const float extra = remaining * child.node->style.flex / flex_sum;
        child.main += extra;
        if (style.row) {
          child.w = child.main;
        } else {
          child.h = child.main;
        }
        flex_extra_total += extra;
      }
    }
  }

  float total_main = fixed_sum + flex_extra_total + gaps;
  for (const ChildLayout& child : children) {
    if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
      total_main += child.margin_main_start + child.margin_main_end;
    }
  }
  float leftover = main_extent - total_main;
  if (leftover < 0.0f) leftover = 0.0f;

  float justify_offset = 0.0f;
  float extra_gap = 0.0f;
  if (style.justify == 1) {
    justify_offset = leftover / 2.0f;
  } else if (style.justify == 2) {
    justify_offset = leftover;
  } else if (style.justify == 3 && children.size() > 1) {
    extra_gap = leftover / static_cast<float>(children.size() - 1);
  }

  float cursor = (style.row ? content_x : content_y) + justify_offset;
  for (ChildLayout& child : children) {
    cursor += child.margin_main_start;
    const int align = EffectiveAlign(style, child.node->style);
    float child_x = 0.0f;
    float child_y = 0.0f;
    if (style.row) {
      child_y = content_y;
      const float cross_space =
          content_h - child.cross - child.margin_cross_start -
          child.margin_cross_end;
      if (align == 2) {
        child_y += child.margin_cross_start + cross_space / 2.0f;
      } else if (align == 3) {
        child_y += child.margin_cross_start + cross_space;
      } else {
        child_y += child.margin_cross_start;
      }
      child_x = cursor;
    } else {
      child_x = content_x;
      const float cross_space =
          content_w - child.cross - child.margin_cross_start -
          child.margin_cross_end;
      if (align == 2) {
        child_x += child.margin_cross_start + cross_space / 2.0f;
      } else if (align == 3) {
        child_x += child.margin_cross_start + cross_space;
      } else {
        child_x += child.margin_cross_start;
      }
      child_y = cursor;
    }
    LayoutNode(child.node, child_x, child_y, child.w, child.h, metrics,
               abs_x + child_x, abs_y + child_y);
    cursor += child.main + child.margin_main_end + MainGap(style) + extra_gap;
  }
  }  // !style.wrap

  // Absolute/fixed children: positioned from the parent's content box (absolute)
  // or from the viewport (fixed) and excluded from flex sizing above.
  for (AppNode* child : node->children) {
    if (child->released || child->style.display_none ||
        child->style.position == 0) {
      continue;
    }
    const bool fixed = child->style.position == 2;
    const float inset_w =
        fixed ? (g_viewport_width > 0.0
                     ? static_cast<float>(g_viewport_width)
                     : content_w)
              : content_w;
    const float inset_h =
        fixed ? (g_viewport_height > 0.0
                     ? static_cast<float>(g_viewport_height)
                     : content_h)
              : content_h;
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    const bool has_left = ResolveInset(child->style, 3, inset_w, &left);
    const bool has_right = ResolveInset(child->style, 1, inset_w, &right);
    const bool has_top = ResolveInset(child->style, 0, inset_h, &top);
    const bool has_bottom = ResolveInset(child->style, 2, inset_h, &bottom);

    float box_w = -1.0f;
    float box_h = -1.0f;
    if (has_left && has_right && !HasExplicitWidth(child->style)) {
      box_w = std::max(0.0f, inset_w - left - right);
    }
    if (has_top && has_bottom && !HasExplicitHeight(child->style)) {
      box_h = std::max(0.0f, inset_h - top - bottom);
    }
    SizeF measured = MeasureNode(child, box_w >= 0.0f ? box_w : inset_w,
                                 box_w >= 0.0f, box_h >= 0.0f ? box_h : inset_h,
                                 box_h >= 0.0f);
    float child_w = box_w >= 0.0f ? box_w : measured.w;
    float child_h = box_h >= 0.0f ? box_h : measured.h;
    child_w = std::max(child_w, child->style.min_width);
    child_h = std::max(child_h, child->style.min_height);
    child->measured_w = child_w;
    child->measured_h = child_h;

    float child_x = content_x;
    float child_y = content_y;
    if (fixed) {
      // Viewport coordinates, then converted back to the parent's coordinate
      // space because the layout buffer is parent-relative.
      float viewport_x = 0.0f;
      float viewport_y = 0.0f;
      if (has_left) {
        viewport_x = left;
      } else if (has_right) {
        viewport_x = inset_w - right - child_w;
      } else {
        // No horizontal inset: keep the static position (parent content origin).
        viewport_x = abs_x + content_x;
      }
      if (has_top) {
        viewport_y = top;
      } else if (has_bottom) {
        viewport_y = inset_h - bottom - child_h;
      } else {
        viewport_y = abs_y + content_y;
      }
      child_x = viewport_x - abs_x;
      child_y = viewport_y - abs_y;
    } else {
      if (has_left) {
        child_x += left;
      } else if (has_right) {
        child_x += content_w - right - child_w;
      }
      if (has_top) {
        child_y += top;
      } else if (has_bottom) {
        child_y += content_h - bottom - child_h;
      }
    }
    LayoutNode(child, child_x, child_y, child_w, child_h, metrics,
               abs_x + child_x, abs_y + child_y);
  }
}

void WriteLayoutMetrics(const std::vector<NodeMetric>& metrics) {
  for (const NodeMetric& metric : metrics) {
    if (metric.node == nullptr || metric.node->layout_buffer_p == nullptr) {
      continue;
    }
    Dart_Handle buffer =
        Dart_HandleFromPersistent(metric.node->layout_buffer_p);
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(buffer, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      float* values = static_cast<float*>(data);
      values[0] = metric.y;
      values[1] = metric.x;
      values[2] = metric.w;
      values[3] = metric.h;
      values[4] = static_cast<float>(metric.node->style.z_index);
      for (int i = 5; i < 9; i++) values[i] = 0.0f;
      Dart_TypedDataReleaseData(buffer);
    }
  }
}

// -- widget materialization --------------------------------------------------

// Yoga metrics are written before widget construction, so allocate every
// node's 9-float buffer up front and register the stable data pointer.
// Native scroll callbacks.  The static page does not need content-size
// estimation or scroll notifications yet, but `newScrollView` calls
// `asFunction()` on every pointer unconditionally, so they must be valid.
struct NativeGestureRecognizerNative {
  uint32_t handler_id;
  uint32_t gesture_type;
  const char* tag;
  const char** simult_handler_tags;
  uint32_t simult_handler_tag_num;
};

const char kScrollVerticalTag[] = "skyline-scroll-vertical";
// GestureType.kVerticalDrag == 6 in package:skyline's enum.
const NativeGestureRecognizerNative g_scroll_recognizers[] = {
    {1u, 6u, kScrollVerticalTag, nullptr, 0u},
};

extern "C" double ScrollNoopContentSize(intptr_t, int32_t, int32_t, double,
                                       double) {
  return std::nan("");
}
extern "C" void ScrollNoopStartEnd(intptr_t, double top, double left,
                                   double width, double height, uint8_t drag) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] scroll start/end: top=%g left=%g size=%gx%g "
                 "drag=%u\n",
                 top, left, width, height, static_cast<unsigned>(drag));
  }
}
extern "C" void ScrollNoopScroll(intptr_t, double top, double left,
                                 double width, double height, double dx,
                                 double dy, uint8_t drag, double velocity) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] scroll: top=%g left=%g delta=(%g,%g) "
                 "drag=%u velocity=%g\n",
                 top, left, dx, dy, static_cast<unsigned>(drag), velocity);
  }
}
extern "C" void ScrollNoopPtr(intptr_t) {}
extern "C" void ScrollNoopDouble(intptr_t, double) {}
extern "C" void ScrollNoopStatus(intptr_t, int32_t, double) {}

bool AttrBool(const AppNode* node, const char* key, bool fallback) {
  const auto found = node->attrs.find(key);
  if (found == node->attrs.end()) return fallback;
  const std::string& value = found->second;
  return value == "1" || value == "true" || value == "True";
}

double AttrNumber(const AppNode* node, const char* key, double fallback) {
  const auto found = node->attrs.find(key);
  if (found == node->attrs.end()) return fallback;
  double value = fallback;
  if (!ParseNumber(found->second, &value)) return fallback;
  return value;
}

void DeletePersistent(Dart_PersistentHandle* handle) {
  if (*handle != nullptr) {
    Dart_DeletePersistentHandle(*handle);
    *handle = nullptr;
  }
}

// Drops all persistent widget handles for a subtree.  Called before a coarse
// rebuild; `include_layout` also releases the 9-float buffers so new ones can
// be allocated for the rebuilt widgets.
void ResetNodeHandles(AppNode* node, bool include_layout) {
  if (node == nullptr) return;
  DeletePersistent(&node->decorated_box_p);
  DeletePersistent(&node->opacity_widget_p);
  DeletePersistent(&node->clip_widget_p);
  DeletePersistent(&node->text_widget_p);
  DeletePersistent(&node->text_span_p);
  DeletePersistent(&node->yoga_node_p);
  DeletePersistent(&node->scroll_view_p);
  DeletePersistent(&node->transform_p);
  DeletePersistent(&node->input_widget_p);
  node->input_widget = nullptr;
  if (node->builder_p != nullptr || !node->builder_items.empty()) {
    g_builders_by_address.erase(node->address);
    for (auto& entry : node->builder_items) {
      Dart_DeletePersistentHandle(entry.second);
    }
    node->builder_items.clear();
    node->builder_item_nodes.clear();
    node->builder_item_signatures.clear();
    node->builder_pending.clear();
    DeletePersistent(&node->builder_p);
  }
  if (node->image_dl) {
    // Keep the decoded image across rebuilds.  DlImageGPU was created with a
    // null unref queue (software rendering), and its destructor asserts when
    // no queue is available, so images live for the process lifetime.
    g_image_nodes.erase(node->address);
  }
  g_background_image_nodes.erase(node->address);
  if (include_layout) DeletePersistent(&node->layout_buffer_p);
  node->decorated_box = nullptr;
  node->opacity_widget = nullptr;
  node->text_widget = nullptr;
  node->connector = nullptr;
  for (AppNode* child : node->children) {
    ResetNodeHandles(child, include_layout);
  }
}

void PrepareLayoutBuffers(AppNode* node) {
  if (node == nullptr || node->released || node->style.display_none) return;
  if (node->layout_buffer_p == nullptr) {
    Dart_Handle buffer = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
    if (!Dart_IsError(buffer)) {
      Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
      void* data = nullptr;
      intptr_t length = 0;
      Dart_Handle status =
          Dart_TypedDataAcquireData(buffer, &data_type, &data, &length);
      if (!Dart_IsError(status) && data_type == Dart_TypedData_kFloat32 &&
          length == 9 && data != nullptr) {
        Dart_TypedDataReleaseData(buffer);
        node->layout_buffer = buffer;
        node->layout_buffer_p = Dart_NewPersistentHandle(buffer);
      }
    }
  }
  for (AppNode* child : node->children) PrepareLayoutBuffers(child);
}

// Appends the virtualized item nodes of a list/grid subtree.  Builder elements
// (`listbuilder`/`gridbuilder`) are transparent containers; their items are the
// actual children.  Item wrappers (`*builderitem`) are real items.
void CollectListItems(AppNode* parent, std::vector<AppNode*>* out) {
  for (AppNode* child : parent->children) {
    if (child->released || child->style.display_none) continue;
    if (child->name == "listbuilder" || child->name == "gridbuilder") {
      CollectListItems(child, out);
    } else {
      out->push_back(child);
    }
  }
}

bool IsListViewName(const std::string& name) {
  return name == "listview" || name == "list-view";
}

bool IsGridViewName(const std::string& name) {
  return name == "gridview" || name == "grid-view";
}

bool IsListBuilderName(const std::string& name) {
  return name == "listbuilder" || name == "list-builder";
}

bool IsGridBuilderName(const std::string& name) {
  return name == "gridbuilder" || name == "grid-builder";
}

// True when `node` is a list/grid builder or lives inside one.
bool IsBuilderName(const std::string& name);
bool IsBuilderSubtree(const AppNode* node) {
  for (const AppNode* current = node; current != nullptr;
       current = current->parent) {
    if (IsBuilderName(current->name)) return true;
  }
  return false;
}

bool IsBuilderName(const std::string& name) {
  return IsListBuilderName(name) || IsGridBuilderName(name);
}

bool IsScrollViewName(const std::string& name) {
  return name == "scrollview" || name == "scroll-view";
}

bool IsImageName(const std::string& name) {
  return name == "image" || name == "wx-image" || name == "img" ||
         name == "cover-image" || name == "wx-cover-image";
}

void DirectRenderImageDispose(intptr_t /*ptr*/) {}
void DirectRenderImageCreate(intptr_t /*ptr*/) {}
void DirectRenderImageLayout(intptr_t /*ptr*/, double /*w*/, double /*h*/) {}

int ImageRepeatMode(const AppNode* node) {
  static const char* kKeys[] = {"repeat", "background-repeat", "backgroundRepeat"};
  for (const char* key : kKeys) {
    const auto found = node->attrs.find(key);
    if (found != node->attrs.end() && !found->second.empty()) {
      return ParseBackgroundRepeat(found->second);
    }
  }
  return node->style.background_repeat;
}

// Draws `image` into a width x height box, applying a CSS-like object-fit and
// repeat mode.  Shared by <image> direct-render widgets and CSS background
// images.
void DrawImageInBox(flutter::Canvas* canvas, const sk_sp<flutter::DlImage>& image,
                    double width, double height, int fit, int repeat,
                    double offset_x, double offset_y) {
  if (!image || !canvas || canvas->builder() == nullptr) return;
  const double image_width = image->width();
  const double image_height = image->height();
  if (image_width <= 0.0 || image_height <= 0.0) return;
  const double target_w = std::max(0.0, width);
  const double target_h = std::max(0.0, height);
  if (target_w <= 0.0 || target_h <= 0.0) return;

  double dest_x = 0.0;
  double dest_y = 0.0;
  double dest_w = target_w;
  double dest_h = target_h;
  if (fit == 1 || fit == 3 || fit == 4) {
    double scale = fit == 3 ? 1.0 : std::min(target_w / image_width,
                                             target_h / image_height);
    if (fit == 4 && image_width <= target_w && image_height <= target_h) {
      scale = 1.0;
    }
    dest_w = image_width * scale;
    dest_h = image_height * scale;
    dest_x = (target_w - dest_w) / 2.0;
    dest_y = (target_h - dest_h) / 2.0;
  } else if (fit == 2) {
    const double scale = std::max(target_w / image_width,
                                  target_h / image_height);
    dest_w = image_width * scale;
    dest_h = image_height * scale;
    dest_x = (target_w - dest_w) / 2.0;
    dest_y = (target_h - dest_h) / 2.0;
  }

  canvas->save();
  canvas->translate(offset_x, offset_y);
  if (fit == 2 || repeat != 0) {
    canvas->clipRect(0.0, 0.0, target_w, target_h,
                     flutter::DlClipOp::kIntersect);
  }
  if (repeat == 0) {
    canvas->builder()->DrawImageRect(
        image, flutter::DlRect::MakeLTRB(0.0, 0.0, image_width, image_height),
        flutter::DlRect::MakeLTRB(dest_x, dest_y, dest_x + dest_w,
                                  dest_y + dest_h),
        flutter::DlImageSampling::kLinear, nullptr);
  } else {
    // CSS repeat tiles at the natural image size (or the background-size
    // result when a fit was requested).
    const double tile_w = fit == 0 ? image_width : dest_w;
    const double tile_h = fit == 0 ? image_height : dest_h;
    if (tile_w <= 0.0 || tile_h <= 0.0) {
      canvas->restore();
      return;
    }
    const bool tile_x = repeat == 1 || repeat == 2;
    const bool tile_y = repeat == 1 || repeat == 3;
    const int columns = tile_x ? static_cast<int>(std::ceil(target_w / tile_w))
                               : 1;
    const int rows = tile_y ? static_cast<int>(std::ceil(target_h / tile_h)) : 1;
    for (int row = 0; row < rows; row++) {
      for (int column = 0; column < columns; column++) {
        const double tx = tile_x ? column * tile_w : dest_x;
        const double ty = tile_y ? row * tile_h : dest_y;
        canvas->builder()->DrawImageRect(
            image, flutter::DlRect::MakeLTRB(0.0, 0.0, image_width, image_height),
            flutter::DlRect::MakeLTRB(tx, ty, tx + tile_w, ty + tile_h),
            flutter::DlImageSampling::kLinear, nullptr);
      }
    }
  }
  canvas->restore();
}

void DirectRenderImagePaint(intptr_t ptr, Dart_Handle canvas_handle,
                            double width, double height, double offset_x,
                            double offset_y) {
  auto found = g_image_nodes.find(ptr);
  if (found == g_image_nodes.end() || found->second == nullptr) return;
  AppNode* node = found->second;
  if (!node->image_dl) return;

  auto* canvas = tonic::DartConverter<flutter::Canvas*>::FromDart(canvas_handle);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] image paint: ptr=0x%lx canvas=%p %dx%d "
                 "size=%gx%g at (%g,%g)\n",
                 static_cast<long>(ptr), static_cast<void*>(canvas),
                 node->image_dl->width(), node->image_dl->height(), width, height,
                 offset_x, offset_y);
  }
  if (canvas == nullptr || canvas->builder() == nullptr) {
    return;
  }

  // Resolve `objectFit` (`mode`/`objectFit` attributes or CSS object-fit).
  int fit = node->style.object_fit;
  static const char* kFitKeys[] = {"objectFit", "object-fit", "mode"};
  for (const char* key : kFitKeys) {
    const auto found = node->attrs.find(key);
    if (found == node->attrs.end() || found->second.empty()) continue;
    const std::string value = found->second;
    if (value == "aspectFit" || value == "contain") {
      fit = 1;
    } else if (value == "aspectFill" || value == "cover") {
      fit = 2;
    } else if (value == "center" || value == "none") {
      fit = 3;
    } else if (value == "scaleToFill" || value == "fill") {
      fit = 0;
    } else if (value == "widthFix" || value == "heightFix") {
      fit = 0;  // the size itself already encodes the preserved aspect ratio
    }
    break;
  }
  DrawImageInBox(canvas, node->image_dl, width, height, fit, ImageRepeatMode(node),
                 offset_x, offset_y);
}

// CSS background-image painting through NewDecoratedBox's onPaint callback.
void BackgroundImageCreate(intptr_t /*ptr*/) {}
void BackgroundImageDisposed(intptr_t /*ptr*/) {}

void BackgroundImagePaint(intptr_t ptr, Dart_Handle canvas_handle, double left,
                          double top, double right, double bottom,
                          Dart_Handle /*path*/) {
  auto found = g_background_image_nodes.find(ptr);
  if (found == g_background_image_nodes.end() || found->second == nullptr) return;
  AppNode* node = found->second;
  if (!node->background_image_dl) return;
  auto* canvas = tonic::DartConverter<flutter::Canvas*>::FromDart(canvas_handle);
  if (canvas == nullptr || canvas->builder() == nullptr) return;
  const double width = std::max(0.0, right - left);
  const double height = std::max(0.0, bottom - top);
  DrawImageInBox(canvas, node->background_image_dl, width, height,
                 node->style.background_fit, node->style.background_repeat, left,
                 top);
}

// `package:skyline` registers this Dart callback; native calls it to perform
// the actual HTTP request (dart:io) and delivers the response through the
// supplied C function pointer.
using RequestHttpResourceFn = void (*)(uint32_t request_context_id,
                                       Dart_Handle url,
                                       void* native_callback);
using CancelRequestHttpResourceFn = void (*)(uint32_t request_context_id);
// `SvgToRgba` renders SVG through package:flutter_svg on the Dart side and
// returns raw RGBA bytes to the native callback.
using SvgToRgbaCallbackFn = void (*)(intptr_t runtime_ptr, intptr_t ptr,
                                      int32_t id, Dart_Handle data,
                                      int32_t width, int32_t height);
using SvgToRgbaFn = void (*)(intptr_t ptr, double pixel_ratio, int32_t id,
                             Dart_Handle url, Dart_Handle raw, double width,
                             double height, void* callback);

constexpr uint32_t kSvgToRgbaHash = 543998557u;
constexpr uint32_t kCancelRequestHttpResourceHash = 653300188u;
constexpr uint32_t kBackgroundImageMarkNeedPaintHash = 741124659u;

sk_sp<flutter::DlImage> WrapSkImage(sk_sp<SkImage> sk_image) {
  if (!sk_image) return nullptr;
  flutter::SkiaGPUObject<SkImage> gpu_image = {std::move(sk_image), nullptr};
  return flutter::DlImageGPU::Make(std::move(gpu_image));
}

sk_sp<flutter::DlImage> DecodeEncodedImage(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return nullptr;
  sk_sp<SkData> sk_data = SkData::MakeWithCopy(data, size);
  return WrapSkImage(SkImages::DeferredFromEncodedData(std::move(sk_data)));
}

sk_sp<flutter::DlImage> DecodeRgbaImage(const uint8_t* data, size_t size,
                                        int width, int height) {
  if (data == nullptr || width <= 0 || height <= 0) return nullptr;
  const size_t expected = static_cast<size_t>(width) * height * 4;
  if (size < expected) return nullptr;
  SkImageInfo info =
      SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                        kPremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) return nullptr;
  std::memcpy(bitmap.getPixels(), data, expected);
  return WrapSkImage(SkImages::RasterFromBitmap(bitmap));
}

bool SourceLooksLikeSvg(const std::string& source, const uint8_t* data,
                        size_t size) {
  std::string path = source;
  const size_t query = path.find('?');
  if (query != std::string::npos) path.resize(query);
  if (EndsWith(LowerAscii(path), ".svg")) return true;
  if (data == nullptr || size == 0) return false;
  size_t index = 0;
  while (index < size && std::isspace(static_cast<unsigned char>(data[index]))) {
    index++;
  }
  if (index + 5 <= size && std::memcmp(data + index, "<?xml", 5) == 0) {
    return true;
  }
  return index + 4 <= size && std::memcmp(data + index, "<svg", 4) == 0;
}

void AssignNodeImage(AppNode* node, const sk_sp<flutter::DlImage>& image,
                     bool background) {
  sk_sp<flutter::DlImage>& target =
      background ? node->background_image_dl : node->image_dl;
  if (target && target != image) {
    RetiredImages().push_back(std::move(target));
  }
  target = image;
  node->image_loaded = true;
  node->image_pending = false;
  std::fprintf(stderr, "[skyline-runtime] image #%u: %s (%dx%d)\n", node->id,
               background ? "background image" : "image",
               image ? image->width() : 0, image ? image->height() : 0);
}

void RebuildAppServicePage(RuntimeHolder* holder);

void RepaintAfterImage(RuntimeHolder* holder, bool background,
                       Dart_PersistentHandle decorated_box) {
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, background, decorated_box]() {
    DemoPageApiScope scope;
    if (background && decorated_box != nullptr && holder != nullptr) {
      auto* mark = reinterpret_cast<void (*)(Dart_Handle)>(
          FindCallback(holder->ffi_table, kBackgroundImageMarkNeedPaintHash));
      if (mark != nullptr) {
        mark(Dart_HandleFromPersistent(decorated_box));
        return;
      }
    }
    RebuildAppServicePage(holder);
  });
}

struct PendingImageRequest {
  RuntimeHolder* holder = nullptr;
  std::string source;
  std::vector<AppNode*> nodes;
};

struct PendingSvgRequest {
  AppNode* node = nullptr;
  RuntimeHolder* holder = nullptr;
  bool background = false;
};

std::unordered_map<uint32_t, PendingImageRequest> g_pending_image_requests;
std::unordered_map<std::string, uint32_t> g_pending_image_by_source;
std::unordered_map<int32_t, PendingSvgRequest> g_pending_svg_requests;
uint32_t g_next_image_request_id = 0x40000000u;
int32_t g_next_svg_request_id = 0x30000000;

void RebuildAppServicePage(RuntimeHolder* holder);

void CancelNetworkImage(AppNode* node) {
  if (!node->image_pending || node->image_request_id == 0) return;
  auto found = g_pending_image_requests.find(node->image_request_id);
  if (found != g_pending_image_requests.end()) {
    auto& nodes = found->second.nodes;
    nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());
    if (nodes.empty()) {
      std::fprintf(stderr,
                   "[skyline-runtime] image request %u cancelled (%s)\n",
                   node->image_request_id, found->second.source.c_str());
      g_pending_image_by_source.erase(found->second.source);
      RuntimeHolder* holder = found->second.holder;
      if (holder != nullptr) {
        auto* cancel = reinterpret_cast<CancelRequestHttpResourceFn>(
            FindCallback(holder->ffi_table, kCancelRequestHttpResourceHash));
        if (cancel != nullptr) cancel(node->image_request_id);
      }
      g_pending_image_requests.erase(found);
    }
  }
  node->image_pending = false;
  node->image_request_id = 0;
}

void CancelPendingImage(AppNode* node) {
  if (node == nullptr) return;
  CancelNetworkImage(node);
  node->image_source.clear();
  node->image_loaded = false;
  node->image_failed = false;
  node->image_pending = false;
  if (node->image_dl) RetiredImages().push_back(std::move(node->image_dl));
}

void OnSvgToRgbaResponse(intptr_t /*runtime_ptr*/, intptr_t /*ptr*/, int32_t id,
                         Dart_Handle data, int32_t width, int32_t height) {
  auto found = g_pending_svg_requests.find(id);
  if (found == g_pending_svg_requests.end()) return;
  PendingSvgRequest pending = found->second;
  g_pending_svg_requests.erase(found);
  AppNode* node = pending.node;
  if (node == nullptr || node->released) return;

  std::vector<uint8_t> bytes;
  if (data != nullptr && !Dart_IsNull(data) && !Dart_IsError(data)) {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* raw = nullptr;
    intptr_t length = 0;
    if (!Dart_IsError(Dart_TypedDataAcquireData(data, &type, &raw, &length)) &&
        raw != nullptr && length > 0) {
      bytes.assign(static_cast<uint8_t*>(raw),
                   static_cast<uint8_t*>(raw) + length);
    }
    Dart_TypedDataReleaseData(data);
  }
  sk_sp<flutter::DlImage> image =
      DecodeRgbaImage(bytes.data(), bytes.size(), width, height);
  if (!image) {
    std::fprintf(stderr,
                 "[skyline-runtime] image #%u: SVG payload unusable (%dx%d, "
                 "%zu bytes)\n",
                 node->id, width, height, bytes.size());
    node->image_failed = true;
    node->image_pending = false;
    return;
  }
  CacheImage(node->image_source, image);
  AssignNodeImage(node, image, pending.background);
  RepaintAfterImage(pending.holder, pending.background,
                    pending.background ? node->decorated_box_p : nullptr);
}

// Hands an SVG payload to package:skyline's `SvgToRgba` callback.
bool StartSvgDecode(AppNode* node, const std::string& source,
                    const std::vector<uint8_t>& bytes, bool background,
                    RuntimeHolder* holder) {
  if (holder == nullptr || node == nullptr) return false;
  auto* svg = reinterpret_cast<SvgToRgbaFn>(
      FindCallback(holder->ffi_table, kSvgToRgbaHash));
  if (svg == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] image #%u: SvgToRgba callback missing\n",
                 node->id);
    return false;
  }
  Dart_Handle raw =
      Dart_NewTypedData(Dart_TypedData_kUint8,
                        static_cast<intptr_t>(bytes.size()));
  if (Dart_IsError(raw)) return false;
  Dart_TypedData_Type type = Dart_TypedData_kInvalid;
  void* data = nullptr;
  intptr_t length = 0;
  if (!Dart_IsError(Dart_TypedDataAcquireData(raw, &type, &data, &length)) &&
      data != nullptr) {
    std::memcpy(data, bytes.data(), bytes.size());
  }
  Dart_TypedDataReleaseData(raw);
  Dart_Handle url = Dart_NewStringFromCString(source.c_str());
  if (Dart_IsError(url)) return false;
  const int32_t id = ++g_next_svg_request_id;
  g_pending_svg_requests[id] = PendingSvgRequest{node, holder, background};
  node->image_pending = true;
  node->image_source = source;
  node->image_target_background = background;
  std::fprintf(stderr, "[skyline-runtime] image #%u: SVG %s (request=%d)\n",
               node->id, source.c_str(), id);
  svg(node->address, 1.0, id, url, raw, 0.0, 0.0,
      reinterpret_cast<void*>(&OnSvgToRgbaResponse));
  return true;
}

// Dart's `RequestHttpResource` performs the actual request (dart:io).  The
// response arrives on the UI task runner; decode once and fan the result out to
// every node waiting on that URL.
extern "C" void OnImageResourceResponse(intptr_t /*runtime_ptr*/,
                                        uint32_t request_id, Dart_Handle data,
                                        uint8_t /*from_cache*/) {
  auto found = g_pending_image_requests.find(request_id);
  if (found == g_pending_image_requests.end()) {
    std::fprintf(stderr,
                 "[skyline-runtime] image response for unknown request %u\n",
                 request_id);
    return;
  }
  PendingImageRequest pending = found->second;
  g_pending_image_requests.erase(found);
  g_pending_image_by_source.erase(pending.source);

  std::vector<uint8_t> bytes;
  if (data != nullptr && !Dart_IsNull(data) && !Dart_IsError(data)) {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* raw = nullptr;
    intptr_t length = 0;
    if (!Dart_IsError(Dart_TypedDataAcquireData(data, &type, &raw, &length)) &&
        raw != nullptr && length > 0) {
      bytes.assign(static_cast<uint8_t*>(raw),
                   static_cast<uint8_t*>(raw) + length);
    }
    Dart_TypedDataReleaseData(data);
  }
  if (bytes.empty()) {
    for (AppNode* node : pending.nodes) {
      if (node != nullptr && !node->released && node->image_source == pending.source) {
        node->image_failed = true;
        node->image_pending = false;
      }
    }
    return;
  }
  if (SourceLooksLikeSvg(pending.source, bytes.data(), bytes.size())) {
    // SVG rasterization is per-node (it needs a Dart isolate scope), but the
    // payload is fetched only once.
    for (AppNode* node : pending.nodes) {
      if (node == nullptr || node->released ||
          node->image_source != pending.source) {
        continue;
      }
      node->image_pending = false;
      DemoPageApiScope scope;
      StartSvgDecode(node, pending.source, bytes, node->image_target_background,
                     pending.holder);
    }
    return;
  }

  sk_sp<flutter::DlImage> image = DecodeEncodedImage(bytes.data(), bytes.size());
  if (!image) {
    for (AppNode* node : pending.nodes) {
      if (node != nullptr && !node->released && node->image_source == pending.source) {
        node->image_failed = true;
        node->image_pending = false;
      }
    }
    std::fprintf(stderr,
                 "[skyline-runtime] network payload unusable (%zu bytes)\n",
                 bytes.size());
    return;
  }
  CacheImage(pending.source, image);
  bool any_background = false;
  Dart_PersistentHandle decorated = nullptr;
  bool any = false;
  for (AppNode* node : pending.nodes) {
    if (node == nullptr || node->released ||
        node->image_source != pending.source) {
      continue;
    }
    AssignNodeImage(node, image, node->image_target_background);
    any = true;
    if (node->image_target_background) {
      any_background = true;
      decorated = node->decorated_box_p;
    }
  }
  std::fprintf(stderr,
               "[skyline-runtime] network image (%zu bytes) for %zu node(s), "
               "rebuilding page\n",
               bytes.size(), pending.nodes.size());
  if (any) RepaintAfterImage(pending.holder, any_background, decorated);
}

bool StartNetworkImage(AppNode* node, const std::string& source,
                       bool background, RuntimeHolder* holder) {
  if (node->image_pending) return false;
  void* request = holder != nullptr
                      ? FindCallback(holder->ffi_table, kRequestHttpResourceHash)
                      : nullptr;
  if (request == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] image #%u: RequestHttpResource missing\n",
                 node->id);
    node->image_failed = true;
    return false;
  }
  node->image_source = source;
  node->image_target_background = background;
  node->image_pending = true;

  auto pending = g_pending_image_by_source.find(source);
  if (pending != g_pending_image_by_source.end()) {
    auto& entry = g_pending_image_requests[pending->second];
    if (std::find(entry.nodes.begin(), entry.nodes.end(), node) ==
        entry.nodes.end()) {
      entry.nodes.push_back(node);
    }
    node->image_request_id = pending->second;
    std::fprintf(stderr,
                 "[skyline-runtime] image #%u: join pending fetch %s "
                 "(request=%u)\n",
                 node->id, source.c_str(), pending->second);
    return false;
  }

  const uint32_t request_id = ++g_next_image_request_id;
  node->image_request_id = request_id;
  PendingImageRequest entry;
  entry.holder = holder;
  entry.source = source;
  entry.nodes.push_back(node);
  g_pending_image_requests[request_id] = entry;
  g_pending_image_by_source[source] = request_id;
  std::fprintf(stderr, "[skyline-runtime] image #%u: fetch %s (request=%u)\n",
               node->id, source.c_str(), request_id);
  // Start the request from a follow-up UI task: the Dart-side
  // `RequestHttpResource` is an async callback and must not be entered from
  // the middle of page construction.
  auto runner = UiTaskRunner();
  if (!runner) runner = g_ui_task_runner;
  if (runner) {
    runner->PostTask([holder, request_id, source]() {
      DemoPageApiScope scope;
      void* request = FindCallback(
          holder != nullptr ? holder->ffi_table : nullptr,
          kRequestHttpResourceHash);
      if (request == nullptr) {
        std::fprintf(stderr,
                     "[skyline-runtime] image request %u: "
                     "RequestHttpResource missing\n",
                     request_id);
        return;
      }
      Dart_Handle url = Dart_NewStringFromCString(source.c_str());
      std::fprintf(stderr, "[skyline-runtime] image request %u starting\n",
                   request_id);
      reinterpret_cast<RequestHttpResourceFn>(request)(
          request_id, url,
          reinterpret_cast<void*>(&OnImageResourceResponse));
    });
  }
  return false;
}

bool LoadImageSource(AppNode* node, const std::string& source, bool background,
                     RuntimeHolder* holder) {
  if (source.empty()) {
    std::fprintf(stderr, "[skyline-runtime] image #%u: no source\n", node->id);
    node->image_failed = true;
    return false;
  }
  auto cached = ImageCache().find(source);
  if (cached != ImageCache().end() && cached->second) {
    AssignNodeImage(node, cached->second, background);
    std::fprintf(stderr, "[skyline-runtime] image #%u: cache hit %s\n",
                 node->id, source.c_str());
    return true;
  }

  if (StartsWith(source, "http://") || StartsWith(source, "https://")) {
    return StartNetworkImage(node, source, background, holder);
  }

  const std::string resolved = ResolveResourcePath(source);
  std::vector<uint8_t> bytes;
  if (!ReadResourceBytes(resolved, &bytes) || bytes.empty()) {
    std::fprintf(stderr, "[skyline-runtime] image #%u: cannot read %s\n",
                 node->id, resolved.c_str());
    node->image_failed = true;
    return false;
  }
  if (SourceLooksLikeSvg(source, bytes.data(), bytes.size())) {
    return StartSvgDecode(node, source, bytes, background, holder);
  }
  sk_sp<flutter::DlImage> image = DecodeEncodedImage(bytes.data(), bytes.size());
  if (!image) {
    std::fprintf(stderr, "[skyline-runtime] image #%u: cannot decode %s\n",
                 node->id, source.c_str());
    node->image_failed = true;
    return false;
  }
  CacheImage(source, image);
  AssignNodeImage(node, image, background);
  if (background) {
    std::fprintf(stderr,
                 "[skyline-runtime] background image #%u: %s -> %s (%zu bytes, "
                 "%dx%d)\n",
                 node->id, source.c_str(), resolved.c_str(), bytes.size(),
                 image->width(), image->height());
  } else {
    std::fprintf(stderr,
                 "[skyline-runtime] image #%u: %s -> %s (%zu bytes, %dx%d)\n",
                 node->id, source.c_str(), resolved.c_str(), bytes.size(),
                 image->width(), image->height());
  }
  return true;
}

bool LoadImageForNode(AppNode* node) {
  if (node->image_loaded) return true;
  if (node->image_failed || node->image_pending) return false;
  static const char* kSourceKeys[] = {"src", "url", "imageUrl", "image",
                                      "path"};
  std::string source;
  for (const char* key : kSourceKeys) {
    const auto found = node->attrs.find(key);
    if (found != node->attrs.end() && !found->second.empty()) {
      source = found->second;
      break;
    }
  }
  if (source.empty()) {
    std::fprintf(stderr, "[skyline-runtime] image #%u: no src attribute\n",
                 node->id);
    node->image_failed = true;
    return false;
  }
  RuntimeHolder* holder = g_page != nullptr ? g_page->holder : nullptr;
  return LoadImageSource(node, source, false, holder);
}

bool LoadBackgroundImageForNode(AppNode* node) {
  if (node == nullptr || node->style.background_image.empty()) return false;
  if (node->background_image_dl != nullptr || node->image_failed ||
      node->image_pending) {
    return node->background_image_dl != nullptr;
  }
  RuntimeHolder* holder = g_page != nullptr ? g_page->holder : nullptr;
  return LoadImageSource(node, node->style.background_image, true, holder);
}

// Builder nodes are slivers.  The Dart side calls back with `(address, index)`
// whenever the sliver needs an item; the callback schedules the mount on the
// UI runner and materializes the captured item node on demand.
void OnBuilderBuildChild(intptr_t address, int32_t index);
void OnBuilderDisposeChild(intptr_t address, int32_t index);

// Builds the `List<BoxShadow>` argument for NewDecoratedBox.
Dart_Handle BuildLinearGradientHandle(const AppStyle& style) {
  if (!style.has_gradient || style.gradient_colors.size() < 2 ||
      g_page == nullptr || g_page->holder == nullptr) {
    return Dart_Null();
  }
  auto* new_linear = reinterpret_cast<NewLinearGradientFn>(
      FindCallback(g_page->holder->ffi_table, kNewLinearGradientHash));
  if (new_linear == nullptr) return Dart_Null();
  Dart_Handle colors =
      Dart_NewList(static_cast<intptr_t>(style.gradient_colors.size()));
  if (Dart_IsError(colors)) return Dart_Null();
  std::vector<GradientStopNative> stops;
  stops.reserve(style.gradient_colors.size());
  for (size_t i = 0; i < style.gradient_colors.size(); i++) {
    Dart_ListSetAt(colors, static_cast<intptr_t>(i),
                   Dart_NewInteger(
                       static_cast<int64_t>(style.gradient_colors[i])));
    double stop = i < style.gradient_stops.size()
                      ? style.gradient_stops[i]
                      : (style.gradient_colors.size() > 1
                             ? static_cast<double>(i) /
                                   static_cast<double>(style.gradient_colors.size() - 1)
                             : 0.0);
    stops.push_back(GradientStopNative{stop, 1});
  }
  return new_linear(style.gradient_angle, colors, stops.data(), -1.0, -1.0,
                    0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0);
}

Dart_Handle BuildBoxShadowList(const AppStyle& style) {  if (!style.has_shadow || g_page == nullptr || g_page->holder == nullptr) {
    return Dart_Null();
  }
  auto* new_box_shadow = reinterpret_cast<NewBoxShadowFn>(
      FindCallback(g_page->holder->ffi_table, kNewBoxShadowHash));
  if (new_box_shadow == nullptr) return Dart_Null();
  Dart_Handle shadow = new_box_shadow(
      style.shadow_color, static_cast<double>(style.shadow_offset_x),
      static_cast<double>(style.shadow_offset_y),
      static_cast<double>(style.shadow_blur),
      static_cast<double>(style.shadow_spread), style.shadow_inset ? 1 : 0);
  if (Dart_IsError(shadow) || Dart_IsNull(shadow)) return Dart_Null();
  Dart_Handle list = Dart_NewList(1);
  if (Dart_IsError(list)) return Dart_Null();
  Dart_ListSetAt(list, 0, shadow);
  return list;
}

// -- native input handle -----------------------------------------------------
//
// The replay renders text nodes directly, but the real SkylineInput widget is
// still created (off-tree) so the native Input* callbacks have a valid Dart
// handle.  This is what makes InputHoldKeyboard / InputCursor /
// InputSelectionStart/End observable instead of log-only.
void InputNoopMarkDirty(intptr_t) {}
void InputNoopOnInput(intptr_t, Dart_Handle, uint32_t, uint32_t) {}
void InputNoopOnFocus(intptr_t, Dart_Handle, uint32_t) {}
void InputNoopOnBlur(intptr_t, Dart_Handle, Dart_Handle, Dart_Handle) {}
void InputNoopOnLineChange(intptr_t, uint32_t, uint32_t, uint32_t) {}
void InputNoopOnConfirm(intptr_t, Dart_Handle) {}
void InputNoopOnKeyboardHeightChange(intptr_t, uint32_t, uint32_t) {}
void InputNoopOnSelectionChange(intptr_t, uint32_t, uint32_t) {}
void InputNoopOnComposition(intptr_t, Dart_Handle) {}
void InputNoopOnPageBottomPaddingChange(intptr_t, double) {}

Dart_Handle CreateInputStateWidget(NewInputFn newInput, AppNode* node) {
  if (newInput == nullptr || node == nullptr) return nullptr;
  Dart_Handle value = Dart_NewStringFromCString(node->text.c_str());
  Dart_Handle type = Dart_NewStringFromCString("text");
  Dart_Handle placeholder = Dart_NewStringFromCString("");
  Dart_Handle confirm = Dart_NewStringFromCString("done");
  if (Dart_IsError(value) || Dart_IsError(type) || Dart_IsError(placeholder) ||
      Dart_IsError(confirm)) {
    return nullptr;
  }
  const AppStyle& style = node->style;
  const double line_height =
      style.line_height > 0.0 ? style.line_height / style.font_size
                              : style.line_height_factor;
  const double letter_spacing =
      std::isnan(style.letter_spacing) ? std::nan("") : style.letter_spacing;
  const uint8_t focus =
      g_page != nullptr && g_page->focused_input_id == node->id ? 1 : 0;
  return newInput(
      node->address, reinterpret_cast<void*>(&InputNoopMarkDirty), 1, node->id,
      0 /* multiLine */, value, type, 0 /* password */, placeholder,
      style.font_size, static_cast<uint8_t>(style.font_weight), 0xFF999999u,
      0 /* disable */, -1 /* maxLength */, 0 /* cursorSpacing */, 0xFF000000u,
      focus, 0 /* autoHeight */, confirm, 0 /* confirmHold */,
      node->input_cursor, node->input_selection_start,
      node->input_selection_end, 0 /* showConfirmBar */, 1 /* adjustPosition */,
      node->input_hold_keyboard ? 1 : 0, static_cast<uint8_t>(style.text_align),
      style.color, 0 /* backgroundColor */, 0 /* textDecoration */,
      0 /* textDecorationStyle */, 0u /* decorationColor */,
      0.0 /* decorationThickness */, nullptr, 0, style.font_size, line_height,
      static_cast<uint8_t>(style.font_weight),
      static_cast<uint8_t>(style.font_style), Dart_Null(), letter_spacing,
      std::nan(""), Dart_Null(), reinterpret_cast<void*>(&InputNoopOnInput),
      reinterpret_cast<void*>(&InputNoopOnFocus),
      reinterpret_cast<void*>(&InputNoopOnBlur),
      reinterpret_cast<void*>(&InputNoopOnLineChange),
      reinterpret_cast<void*>(&InputNoopOnConfirm),
      reinterpret_cast<void*>(&InputNoopOnKeyboardHeightChange),
      reinterpret_cast<void*>(&InputNoopOnSelectionChange),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnPageBottomPaddingChange));
}

Dart_Handle BuildWidget(AppNode* node, NewYGNodeFn newYGNode,
                        NewYGConnectorFn newYGConnector, NewTextFn newText,
                        NewTextSpanFn newTextSpan,
                        NewDecoratedBoxFn newDecoratedBox,
                        NewOpacityFn newOpacity, NewScrollViewFn newScrollView,
                        NewListViewFn newListView, NewGridViewFn newGridView,
                        NewDirectRenderWidgetFn newDirectRenderWidget,
                        NewListBuilderFn newListBuilder,
                        NewGridBuilderFn newGridBuilder, bool is_root) {
  if (node->released || node->style.display_none) {
    return Dart_Null();
  }
  const AppStyle& style = node->style;
  Dart_Handle inner = Dart_Null();

  if (node->tag == "text") {
    if (node->name == "input" && g_page != nullptr &&
        g_page->holder != nullptr) {
      auto* newInput = reinterpret_cast<NewInputFn>(FindCallback(
          g_page->holder->ffi_table, kNewInputHash));
      if (newInput != nullptr) {
        DeletePersistent(&node->input_widget_p);
        Dart_Handle input_widget = CreateInputStateWidget(newInput, node);
        if (input_widget != nullptr && !Dart_IsError(input_widget) &&
            !Dart_IsNull(input_widget)) {
          node->input_widget = input_widget;
          node->input_widget_p = Dart_NewPersistentHandle(input_widget);
        } else if (Dart_IsError(input_widget)) {
          ReportDartError("appservice NewInput", input_widget);
        }
      }
    }
    std::string text_storage;
    const char* text = NodeText(node, &text_storage);
    // NewTextSpan's `height` is the Flutter line-height multiplier, while CSS
    // line-height is in pixels; convert back to a multiplier.
    const double line_height =
        style.line_height > 0.0 ? style.line_height / style.font_size
                                : style.line_height_factor;
    Dart_Handle span_handle = nullptr;
    inner = CreateTextWidget(
        newText, newTextSpan, node->address, text, style.font_size, style.color,
        style.font_weight, style.font_style,
        std::isnan(style.letter_spacing) ? std::nan("") : style.letter_spacing,
        line_height, style.text_align, static_cast<int>(style.max_lines),
        &span_handle);
    if (Dart_IsError(inner)) {
      ReportDartError("appservice text widget", inner);
      return Dart_Null();
    }
    node->text_widget = inner;
    node->text_widget_p = Dart_NewPersistentHandle(inner);
    if (span_handle != nullptr) {
      node->text_span_p = Dart_NewPersistentHandle(span_handle);
    }
  } else if (IsImageName(node->name) && newDirectRenderWidget != nullptr) {
    // Network images are fetched asynchronously; the widget is installed
    // immediately and repaints after the page is rebuilt on response.
    LoadImageForNode(node);
    Dart_Handle widget = newDirectRenderWidget(
        node->address, reinterpret_cast<void*>(&DirectRenderImagePaint),
        reinterpret_cast<void*>(&DirectRenderImageDispose),
        reinterpret_cast<void*>(&DirectRenderImageCreate),
        reinterpret_cast<void*>(&DirectRenderImageLayout));
    if (Dart_IsError(widget)) {
      ReportDartError("appservice direct render widget", widget);
    } else {
      inner = widget;
      g_image_nodes[node->address] = node;
    }
  } else if (IsBuilderName(node->name) &&
             ((IsGridBuilderName(node->name) && newGridBuilder != nullptr) ||
              (IsListBuilderName(node->name) && newListBuilder != nullptr))) {
    // `<list-builder>`/`<grid-builder>`: a virtualized sliver.  Items are not
    // built here; the sliver requests them through onBuildChild.
    node->builder_kind = IsGridBuilderName(node->name) ? 1 : 0;
    node->builder_child_count = static_cast<int>(
        AttrNumber(node, "childCount", static_cast<double>(AttrNumber(
                                           node, "child-count", 0.0))));
    node->builder_child_height =
        static_cast<float>(AttrNumber(node, "childHeight", 0.0));
    node->builder_cross_axis_count = std::max(
        1, static_cast<int>(AttrNumber(node, "crossAxisCount", 2.0)));
    node->builder_main_spacing =
        static_cast<float>(AttrNumber(node, "mainAxisSpacing", 0.0));
    node->builder_cross_spacing =
        static_cast<float>(AttrNumber(node, "crossAxisSpacing", 0.0));
    node->builder_max_cross_extent =
        static_cast<float>(AttrNumber(node, "maxCrossAxisExtent", 0.0));
    node->builder_background = style.has_background ? style.background : 0u;
    node->builder_synced_child_count = node->builder_child_count;
    node->builder_synced_child_height = node->builder_child_height;
    node->builder_synced_cross_axis_count = node->builder_cross_axis_count;
    node->builder_synced_main_spacing = node->builder_main_spacing;
    node->builder_synced_cross_spacing = node->builder_cross_spacing;
    node->builder_synced_max_cross_extent = node->builder_max_cross_extent;
    node->builder_synced_background = node->builder_background;
    Dart_Handle empty = Dart_NewList(0);
    Dart_Handle builder = Dart_Null();
    if (node->builder_kind == 1) {
      builder = newGridBuilder(
          node->address, empty,
          static_cast<uint8_t>(AttrNumber(node, "type", 0.0)),
          node->builder_child_count, node->builder_cross_axis_count,
          node->builder_main_spacing, node->builder_cross_spacing,
          node->builder_max_cross_extent, style.padding[0], style.padding[1],
          style.padding[2], style.padding[3], node->builder_background,
          reinterpret_cast<void*>(&OnBuilderBuildChild),
          reinterpret_cast<void*>(&OnBuilderDisposeChild));
    } else {
      builder = newListBuilder(
          node->address, empty,
          static_cast<uint8_t>(AttrNumber(node, "type", 0.0)),
          node->builder_child_count, node->builder_child_height,
          style.padding[0], style.padding[1], style.padding[2], style.padding[3],
          node->builder_background,
          reinterpret_cast<void*>(&OnBuilderBuildChild),
          reinterpret_cast<void*>(&OnBuilderDisposeChild));
    }
    if (Dart_IsError(builder) || Dart_IsNull(builder)) {
      ReportDartError("appservice NewListBuilder/NewGridBuilder", builder);
      return Dart_Null();
    }
    node->builder_p = Dart_NewPersistentHandle(builder);
    g_builders_by_address[node->address] = node;
    std::fprintf(stderr,
                 "[skyline-runtime] builder #%u: %s count=%d item=%gx%g\n",
                 node->id, node->builder_kind == 1 ? "grid" : "list",
                 node->builder_child_count,
                 static_cast<double>(node->measured_w),
                 static_cast<double>(node->builder_child_height));
    // Slivers are consumed by the ancestor scroll view; do not wrap in a
    // YGNode (a ParentDataWidget under a sliver is invalid).
    return builder;
  } else {
    const bool is_scroll_view =
        IsScrollViewName(node->name) && newScrollView != nullptr;
    const bool is_list_view =
        IsListViewName(node->name) && newListView != nullptr;
    const bool is_grid_view =
        IsGridViewName(node->name) && newGridView != nullptr;
    if (is_list_view || is_grid_view) {
      // `<list-view>`/`<grid-view>` produce a virtualized sliver.  Its items
      // are top-level YGNodes (a sliver cannot host a ParentDataWidget).
      std::vector<AppNode*> items;
      CollectListItems(node, &items);
      const int grid_type =
          static_cast<int>(AttrNumber(node, "type", 1.0));  // 1 = aligned
      const int cross_axis_count = std::max(
          1, static_cast<int>(AttrNumber(node, "crossAxisCount", 2.0)));
      const double grid_main_spacing = AttrNumber(node, "mainAxisSpacing", 0.0);
      const double grid_cross_spacing =
          AttrNumber(node, "crossAxisSpacing", 0.0);
      float slot_width = node->measured_w > 0.0f ? node->measured_w : 0.0f;
      if (is_grid_view && cross_axis_count > 1 && slot_width > 0.0f) {
        slot_width =
            (slot_width - static_cast<float>(grid_cross_spacing) *
                             static_cast<float>(cross_axis_count - 1)) /
            static_cast<float>(cross_axis_count);
        if (slot_width <= 0.0f) slot_width = node->measured_w;
      }
      std::vector<Dart_Handle> item_widgets;
      item_widgets.reserve(items.size());
      for (AppNode* item : items) {
        // Flutter's slivers position items themselves, so lay each item's
        // subtree out at the origin (the page-wide pass gave it an absolute y).
        if (slot_width > 0.0f) {
          MeasureNode(item, slot_width, true, kInf, false);
        }
        std::vector<NodeMetric> item_metrics;
        LayoutNode(item, 0.0f, 0.0f, item->measured_w, item->measured_h,
                   &item_metrics);
        WriteLayoutMetrics(item_metrics);
        Dart_Handle widget =
            BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                        newDecoratedBox, newOpacity, newScrollView, newListView,
                        newGridView, newDirectRenderWidget, newListBuilder,
                        newGridBuilder, true);
        if (widget != nullptr && !Dart_IsError(widget)) {
          item_widgets.push_back(widget);
        }
      }
      Dart_Handle child_list =
          Dart_NewList(static_cast<intptr_t>(item_widgets.size()));
      for (size_t i = 0; i < item_widgets.size(); i++) {
        Dart_ListSetAt(child_list, static_cast<intptr_t>(i), item_widgets[i]);
      }
      Dart_Handle sliver = Dart_Null();
      if (is_list_view) {
        sliver = newListView(node->address, child_list, style.padding[0],
                             style.padding[1], style.padding[2], style.padding[3],
                             style.has_background ? style.background : 0u,
                             nullptr, nullptr);
      } else {
        sliver = newGridView(
            node->address, child_list, static_cast<uint8_t>(grid_type),
            cross_axis_count, static_cast<float>(grid_main_spacing),
            static_cast<float>(grid_cross_spacing),
            static_cast<float>(AttrNumber(node, "maxCrossAxisExtent", 0.0)),
            style.padding[0], style.padding[1], style.padding[2], style.padding[3],
            style.has_background ? style.background : 0u);
      }
      if (Dart_IsError(sliver)) {
        ReportDartError("appservice list/grid view", sliver);
        sliver = Dart_Null();
      } else if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[ops-debug] %s #%u: %zu items\n",
                     is_list_view ? "listview" : "gridview", node->id,
                     item_widgets.size());
      }
      inner = sliver;

      const bool inside_scroll =
          node->parent != nullptr && IsScrollViewName(node->parent->name);
      if (inside_scroll) {
        // The parent scroll view consumes this widget as a sliver directly.
        return inner;
      }
      // Standalone list/grid: host the sliver in a customScrollView so it can
      // scroll like a regular scroll-view.
      if (newScrollView != nullptr && !Dart_IsError(inner)) {
        Dart_Handle children = Dart_NewList(1);
        Dart_ListSetAt(children, 0, inner);
        Dart_Handle scroll = newScrollView(
            node->address, node->id, 3 /* ScrollViewType.custom */, 0, 1, 0.0,
            0.0, 0, 50.0, 50.0, 0, 0.0, 0u, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 1,
            1, 0, 0, 0, 0.0,
            const_cast<NativeGestureRecognizerNative*>(g_scroll_recognizers), 1,
            Dart_Null(), 0, 0.0, style.padding[0], style.padding[1],
            style.padding[2], style.padding[3], 0, 0, 1, 0,
            reinterpret_cast<void*>(&ScrollNoopContentSize),
            reinterpret_cast<void*>(&ScrollNoopStartEnd),
            reinterpret_cast<void*>(&ScrollNoopScroll),
            reinterpret_cast<void*>(&ScrollNoopStartEnd),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopStatus),
            reinterpret_cast<void*>(&ScrollNoopPtr), children);
        if (Dart_IsError(scroll)) {
          ReportDartError("appservice standalone list scroll view", scroll);
        } else {
          inner = scroll;
          node->scroll_view_p = Dart_NewPersistentHandle(scroll);
        }
      }
    } else {
      std::vector<Dart_Handle> child_widgets;
      child_widgets.reserve(node->children.size());
      bool has_sliver_child = false;
      for (AppNode* child : node->children) {
        if (child->released || child->style.display_none) continue;
        // position:fixed is promoted to the page root connector so scrolling
        // ancestors do not move it.
        if (DeferFixedChild(child)) continue;
        if (IsListViewName(child->name) || IsGridViewName(child->name) ||
            IsBuilderName(child->name)) {
          has_sliver_child = true;
        }
        // A scroll view's children live under Flutter's ListView, not under a
        // YGConnector, so they must be top-level YGNodes (no ParentDataWidget).
        Dart_Handle widget =
            BuildWidget(child, newYGNode, newYGConnector, newText, newTextSpan,
                        newDecoratedBox, newOpacity, newScrollView, newListView,
                        newGridView, newDirectRenderWidget, newListBuilder,
                        newGridBuilder, is_scroll_view);
        if (widget == nullptr) continue;
        child_widgets.push_back(widget);
      }
      // Fixed nodes were skipped above.  At the page root they become direct
      // children of the root connector and use viewport-relative layout.
      if (node == g_fixed_root_node && g_collect_fixed_nodes) {
        for (size_t i = 0; i < g_deferred_fixed_nodes.size(); i++) {
          AppNode* fixed = g_deferred_fixed_nodes[i];
          if (fixed == nullptr || fixed->released ||
              fixed->style.display_none) {
            continue;
          }
          std::vector<NodeMetric> fixed_metrics;
          fixed_metrics.push_back(NodeMetric{fixed, fixed->abs_x, fixed->abs_y,
                                             fixed->measured_w,
                                             fixed->measured_h});
          WriteLayoutMetrics(fixed_metrics);
          Dart_Handle widget =
              BuildWidget(fixed, newYGNode, newYGConnector, newText,
                          newTextSpan, newDecoratedBox, newOpacity, newScrollView,
                          newListView, newGridView, newDirectRenderWidget,
                          newListBuilder, newGridBuilder, false);
          if (widget != nullptr && !Dart_IsError(widget)) {
            child_widgets.push_back(widget);
          }
        }
      }
      if (is_scroll_view) {
        Dart_Handle list =
            Dart_NewList(static_cast<intptr_t>(child_widgets.size()));
        for (size_t i = 0; i < child_widgets.size(); i++) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), child_widgets[i]);
        }
        const bool scroll_x_attr = AttrBool(node, "scrollX", false);
        const bool scroll_y_attr = AttrBool(node, "scrollY", false);
        // List/grid slivers require a CustomScrollView; plain box children are
        // wrapped in SliverToBoxAdapter by SkylineScrollView.
        const uint8_t scroll_type = has_sliver_child ? 3 : 0;
        if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
          std::fprintf(stderr,
                       "[ops-debug] scrollview #%u scrollX=%d scrollY=%d "
                       "scrollEnabled=%d type=%u (slivers=%d)\n",
                       node->id, scroll_x_attr ? 1 : 0, scroll_y_attr ? 1 : 0,
                       AttrBool(node, "scrollEnabled", true) ? 1 : 0,
                       static_cast<unsigned>(scroll_type),
                       has_sliver_child ? 1 : 0);
        }
        Dart_Handle scroll = newScrollView(
            node->address, node->id, scroll_type, scroll_x_attr ? 1 : 0,
            scroll_y_attr ? 1 : 0, AttrNumber(node, "scrollTop", 0.0),
            AttrNumber(node, "scrollLeft", 0.0), 0, 50.0, 50.0, 0, 0.0, 0u, 0,
            0, 0, 0, 0, 0, 0, 0.0, 0.0,
            AttrBool(node, "scrollEnabled", true) ? 1 : 0,
            AttrBool(node, "bounces", true) ? 1 : 0, 0, 0, 0, 0.0,
            const_cast<NativeGestureRecognizerNative*>(g_scroll_recognizers), 1,
            Dart_Null(), 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0,
            0 /* paint order */, 1 /* scrollAnchoring */,
            0 /* innerCorrectionMode */,
            reinterpret_cast<void*>(&ScrollNoopContentSize),
            reinterpret_cast<void*>(&ScrollNoopStartEnd),
            reinterpret_cast<void*>(&ScrollNoopScroll),
            reinterpret_cast<void*>(&ScrollNoopStartEnd),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopDouble),
            reinterpret_cast<void*>(&ScrollNoopStatus),
            reinterpret_cast<void*>(&ScrollNoopPtr), list);
        if (Dart_IsError(scroll)) {
          ReportDartError("appservice NewScrollView", scroll);
        } else {
          inner = scroll;
          node->scroll_view_p = Dart_NewPersistentHandle(scroll);
        }
      } else if (!child_widgets.empty()) {
        Dart_Handle list =
            Dart_NewList(static_cast<intptr_t>(child_widgets.size()));
        for (size_t i = 0; i < child_widgets.size(); i++) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), child_widgets[i]);
        }
        Dart_Handle connector = newYGConnector(0, list);
        if (Dart_IsError(connector)) {
          ReportDartError("appservice NewYGConnector", connector);
        } else {
          inner = connector;
          node->connector = connector;
        }
      }
    }
    const bool wants_background_image = !style.background_image.empty();
    const bool wants_paint_transition =
        (style.transition_mask &
         (kTransitionBackground | kTransitionBorderColor)) != 0;
    const bool has_visual = style.has_background || HasBorder(style) ||
                            HasAnyRadius(style) ||
                            style.has_shadow || wants_background_image ||
                            style.has_gradient || wants_paint_transition;
    if (has_visual) {
      if (wants_background_image) LoadBackgroundImageForNode(node);
      double widths[4] = {0, 0, 0, 0};
      uint32_t colors[4] = {0u, 0u, 0u, 0u};
      uint8_t styles[4] = {0, 0, 0, 0};
      for (int side = 0; side < 4; side++) {
        widths[side] = EffectiveBorderWidth(style, side);
        if (widths[side] > 0.0) {
          colors[side] = EffectiveBorderColor(style, side);
          styles[side] = 1;
        }
      }
      Dart_Handle shadows = BuildBoxShadowList(style);
      Dart_Handle gradient = BuildLinearGradientHandle(style);
      const double radius_tl = EffectiveCornerRadius(style, 0);
      const double radius_tr = EffectiveCornerRadius(style, 1);
      const double radius_br = EffectiveCornerRadius(style, 2);
      const double radius_bl = EffectiveCornerRadius(style, 3);
      const uint8_t radius_tl_pct = CornerRadiusPercent(style, 0) ? 1 : 0;
      const uint8_t radius_tr_pct = CornerRadiusPercent(style, 1) ? 1 : 0;
      const uint8_t radius_br_pct = CornerRadiusPercent(style, 2) ? 1 : 0;
      const uint8_t radius_bl_pct = CornerRadiusPercent(style, 3) ? 1 : 0;
      Dart_Handle decorated = newDecoratedBox(
          node->address, style.background, widths[3], widths[0], widths[1],
          widths[2], colors[3], colors[0], colors[1], colors[2], styles[3],
          styles[0], styles[1], styles[2], radius_tl, radius_tl,
          radius_tr, radius_tr, radius_bl, radius_bl, radius_br, radius_br,
          radius_tl_pct, radius_tl_pct, radius_tr_pct, radius_tr_pct,
          radius_bl_pct, radius_bl_pct, radius_br_pct, radius_br_pct,
          Dart_Null(), 0, 0, 0.0, 0.0,
          0.0, 0.0, static_cast<uint8_t>(wants_background_image
                                             ? style.background_repeat
                                             : 0),
          static_cast<uint8_t>(wants_background_image ? style.background_fit
                                                      : 0),
          0.0, 0.0, 0, 0, gradient, 0, shadows, 0,
          reinterpret_cast<void*>(&BackgroundImagePaint),
          reinterpret_cast<void*>(&BackgroundImageCreate),
          reinterpret_cast<void*>(&BackgroundImageDisposed), nullptr, inner);
      if (Dart_IsError(decorated)) {
        ReportDartError("appservice NewDecoratedBox", decorated);
      } else {
        inner = decorated;
        node->decorated_box = decorated;
        node->decorated_box_p = Dart_NewPersistentHandle(decorated);
        if (wants_background_image) {
          g_background_image_nodes[node->address] = node;
        }
      }
    }
    if (style.opacity < 0.999 ||
        (style.transition_mask & kTransitionOpacity) != 0) {
      Dart_Handle opacity = newOpacity(style.opacity, inner);
      if (Dart_IsError(opacity)) {
        ReportDartError("appservice NewOpacity", opacity);
      } else {
        inner = opacity;
        node->opacity_widget = opacity;
        node->opacity_widget_p = Dart_NewPersistentHandle(opacity);
      }
    }
    if (style.clip && g_page != nullptr && g_page->holder != nullptr) {
      auto* new_clip = reinterpret_cast<NewClipRRectFn>(FindCallback(
          g_page->holder->ffi_table, kNewClipRRectHash));
      if (new_clip != nullptr) {
        const double tl = EffectiveCornerRadius(style, 0);
        const double tr = EffectiveCornerRadius(style, 1);
        const double br = EffectiveCornerRadius(style, 2);
        const double bl = EffectiveCornerRadius(style, 3);
        const uint8_t tl_pct = CornerRadiusPercent(style, 0) ? 1 : 0;
        const uint8_t tr_pct = CornerRadiusPercent(style, 1) ? 1 : 0;
        const uint8_t br_pct = CornerRadiusPercent(style, 2) ? 1 : 0;
        const uint8_t bl_pct = CornerRadiusPercent(style, 3) ? 1 : 0;
        Dart_Handle clip = new_clip(
            0, tl, tl, tr, tr, bl, bl, br, br, tl_pct, tl_pct, tr_pct, tr_pct,
            bl_pct, bl_pct, br_pct, br_pct, EffectiveBorderWidth(style, 3),
            EffectiveBorderWidth(style, 0), EffectiveBorderWidth(style, 1),
            EffectiveBorderWidth(style, 2), inner);
        if (Dart_IsError(clip)) {
          ReportDartError("appservice NewClipRRect", clip);
        } else {
          inner = clip;
          node->clip_widget = clip;
          node->clip_widget_p = Dart_NewPersistentHandle(clip);
        }
      }
    }
  }

  // `concreteWidgetType` drives YGNodeRenderObject.hitTestChildren: containers
  // whose child is a YGConnector must report kYGConnector and pass that
  // connector so shadow hit-testing walks into the child YGNodes.  A plain
  // kYogaNode would stop at the connector's `hitTest` (which only reports
  // `size.contains`), leaving the whole subtree unreachable for gestures.
  constexpr uint32_t kWidgetTypeYogaNode = 1;
  constexpr uint32_t kWidgetTypeYGConnector = 3;
  constexpr uint32_t kWidgetTypeScrollView = 13;
  uint32_t concrete_type = kWidgetTypeYogaNode;
  Dart_Handle yoga_connector = Dart_Null();
  if (node->tag != "text") {
    if (node->name == "scrollview" && newScrollView != nullptr) {
      concrete_type = kWidgetTypeScrollView;
    } else if (node->connector != nullptr) {
      concrete_type = kWidgetTypeYGConnector;
      yoga_connector = node->connector;
    }
  }

  Dart_Handle buffer = node->layout_buffer_p != nullptr
                           ? Dart_HandleFromPersistent(node->layout_buffer_p)
                           : node->layout_buffer;
  if (buffer == nullptr) {
    buffer = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
    if (Dart_IsError(buffer)) {
      ReportDartError("appservice typed data", buffer);
      return Dart_Null();
    }
  }
  Dart_Handle yoga_node = newYGNode(
      node->address, buffer, is_root ? 1 : 0, 0, 0, 0, concrete_type,
      Dart_Null(), 0.0, 0.0, 0.0, 0.0, 1, 0, 0, Dart_Null(), yoga_connector,
      inner);
  if (Dart_IsError(yoga_node)) {
    ReportDartError("appservice NewYGNode", yoga_node);
    return Dart_Null();
  }
  // CSS transform: hand the column-major matrix to the YGNode and rotate/scale
  // around the element centre (CSS transform-origin default).
  if (style.has_transform && g_page != nullptr && g_page->holder != nullptr) {
    auto* setup = reinterpret_cast<YGNodeSetupTransformFn>(
        FindCallback(g_page->holder->ffi_table, kYGNodeSetupTransformHash));
    auto* origin = reinterpret_cast<YGNodeTransformOriginFn>(
        FindCallback(g_page->holder->ffi_table, kYGNodeTransformOriginHash));
    Dart_Handle matrix = Dart_NewTypedData(Dart_TypedData_kFloat64, 16);
    if (setup != nullptr && !Dart_IsError(matrix)) {
      Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
      void* data = nullptr;
      intptr_t length = 0;
      if (!Dart_IsError(Dart_TypedDataAcquireData(matrix, &data_type, &data,
                                                 &length)) &&
          data_type == Dart_TypedData_kFloat64 && length == 16 &&
          data != nullptr) {
        auto* values = static_cast<double*>(data);
        for (int i = 0; i < 16; i++) {
          values[i] = static_cast<double>(style.transform[i]);
        }
      }
      Dart_TypedDataReleaseData(matrix);
      setup(yoga_node, matrix);
      if (origin != nullptr) {
        double origin_x = 0.0;
        double origin_y = 0.0;
        ResolveTransformOrigin(style, node->measured_w, node->measured_h,
                               &origin_x, &origin_y);
        origin(yoga_node, origin_x, origin_y, -1.0, -1.0);
      }
      if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[skyline-runtime] transform #%u matrix=[%g,%g,%g,%g "
                     "%g,%g,%g,%g %g,%g,%g,%g %g,%g,%g,%g]\n",
                     node->id, style.transform[0], style.transform[1],
                     style.transform[2], style.transform[3],
                     style.transform[4], style.transform[5],
                     style.transform[6], style.transform[7],
                     style.transform[8], style.transform[9],
                     style.transform[10], style.transform[11],
                     style.transform[12], style.transform[13],
                     style.transform[14], style.transform[15]);
      }
      node->transform_p = Dart_NewPersistentHandle(matrix);
    }
  }
  node->widget = yoga_node;
  node->yoga_node_p = Dart_NewPersistentHandle(yoga_node);
  return yoga_node;
}

// -- list/grid builder incremental mount -------------------------------------

// Finds the captured node that backs `index`.  Items may declare an explicit
// `index` attribute; otherwise the captured order is the index order.
AppNode* FindBuilderItemNode(AppNode* builder, int index) {
  int sequential = 0;
  for (AppNode* child : builder->children) {
    if (child->released || child->style.display_none) continue;
    if (child->name != "listbuilderitem" && child->name != "gridbuilderitem" &&
        child->name != "list-item" && child->name != "grid-item" &&
        child->name != "builderitem") {
      continue;
    }
    int item_index = sequential;
    const auto attr = child->attrs.find("index");
    if (attr != child->attrs.end()) {
      item_index = std::atoi(attr->second.c_str());
    }
    if (item_index == index) return child;
    sequential++;
  }
  return nullptr;
}

uint64_t HashBytes(uint64_t seed, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; i++) {
    seed ^= bytes[i];
    seed *= 1099511628211ull;
  }
  return seed;
}

uint64_t HashString(uint64_t seed, const std::string& value) {
  seed = HashBytes(seed, value.data(), value.size());
  const uint8_t separator = 0xFF;
  return HashBytes(seed, &separator, 1);
}

// Content signature used to notice builder-item changes without rebuilding the
// whole page.  Attributes are sorted so unordered_map iteration order does not
// leak into the result.
uint64_t NodeContentSignature(const AppNode* node) {
  if (node == nullptr) return 0;
  uint64_t hash = 1469598103934665603ull;
  hash = HashString(hash, node->tag);
  hash = HashString(hash, node->name);
  hash = HashString(hash, node->text);
  hash = HashString(hash, node->inline_style);
  for (const std::string& klass : node->classes) hash = HashString(hash, klass);
  std::vector<std::string> keys;
  keys.reserve(node->attrs.size());
  for (const auto& entry : node->attrs) keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  for (const std::string& key : keys) {
    hash = HashString(hash, key);
    hash = HashString(hash, node->attrs.at(key));
  }
  for (const AppNode* child : node->children) {
    if (child == nullptr || child->released) continue;
    hash = HashBytes(hash, &child->id, sizeof(child->id));
    const uint64_t child_hash = NodeContentSignature(child);
    hash = HashBytes(hash, &child_hash, sizeof(child_hash));
  }
  return hash;
}

void MaterializeBuilderItem(RuntimeHolder* holder, intptr_t address,
                            int index) {
  auto found = g_builders_by_address.find(address);
  if (found == g_builders_by_address.end() || found->second == nullptr) return;
  AppNode* builder = found->second;
  if (builder->released || builder->builder_p == nullptr) return;
  if (builder->builder_items.count(index) != 0) return;
  builder->builder_pending.erase(index);
  AppNode* item = FindBuilderItemNode(builder, index);
  if (item == nullptr) return;

  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* newText =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newScrollView = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  auto* newListView = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  auto* newGridView = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  auto* newDirectRenderWidget = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  auto* newListBuilder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  auto* newGridBuilder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  auto* newItem =
      builder->builder_kind == 1
          ? reinterpret_cast<NewListBuilderItemFn>(
                FindCallback(holder->ffi_table, kNewGridBuilderItemHash))
          : reinterpret_cast<NewListBuilderItemFn>(
                FindCallback(holder->ffi_table, kNewListBuilderItemHash));
  auto* append = reinterpret_cast<BuilderMutateChildFn>(
      FindCallback(holder->ffi_table, builder->builder_kind == 1
                                          ? kGridBuilderAppendChildHash
                                          : kListBuilderAppendChildHash));
  if (newYGNode == nullptr || newYGConnector == nullptr || newText == nullptr ||
      newTextSpan == nullptr || newDecoratedBox == nullptr ||
      newOpacity == nullptr || newItem == nullptr || append == nullptr) {
    return;
  }

  float width = builder->measured_w > 0.0f
                    ? builder->measured_w
                    : static_cast<float>(g_viewport_width);
  if (width <= 0.0f) width = 320.0f;
  if (builder->builder_kind == 1) {
    const int columns = std::max(1, builder->builder_cross_axis_count);
    width = (width - builder->builder_cross_spacing * (columns - 1)) /
            static_cast<float>(columns);
    if (width < 0.0f) width = 0.0f;
  }
  PrepareLayoutBuffers(item);
  MeasureNode(item, width, true, kInf, false);
  if (builder->builder_child_height > 0.0f) {
    item->measured_h = builder->builder_child_height;
  }
  if (item->measured_w <= 0.0f) item->measured_w = width;
  std::vector<NodeMetric> metrics;
  LayoutNode(item, 0.0f, 0.0f, item->measured_w, item->measured_h, &metrics);
  WriteLayoutMetrics(metrics);

  Dart_Handle widget =
      BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                  newDecoratedBox, newOpacity, newScrollView, newListView,
                  newGridView, newDirectRenderWidget, newListBuilder,
                  newGridBuilder, true);
  if (widget == nullptr || Dart_IsError(widget)) return;
  Dart_Handle children = Dart_NewList(1);
  Dart_ListSetAt(children, 0, widget);
  Dart_Handle item_widget = newItem(children, index);
  if (item_widget == nullptr || Dart_IsError(item_widget)) {
    ReportDartError("appservice builder item", item_widget);
    return;
  }
  Dart_Handle builder_widget = Dart_HandleFromPersistent(builder->builder_p);
  Dart_Handle pivot = nullptr;
  int pivot_index = -1;
  for (const auto& entry : builder->builder_items) {
    if (entry.first <= index) continue;
    if (pivot_index < 0 || entry.first < pivot_index) {
      pivot_index = entry.first;
      pivot = Dart_HandleFromPersistent(entry.second);
    }
  }
  auto* insert_before = reinterpret_cast<BuilderInsertBeforeFn>(FindCallback(
      holder->ffi_table, builder->builder_kind == 1
                             ? kGridBuilderInsertBeforeHash
                             : kListBuilderInsertBeforeHash));
  if (pivot != nullptr && insert_before != nullptr) {
    insert_before(builder_widget, item_widget, pivot);
    std::fprintf(stderr,
                 "[skyline-runtime] %s: #%u index=%d before=%d\n",
                 builder->builder_kind == 1 ? "GridBuilderInsertBefore"
                                            : "ListBuilderInsertBefore",
                 builder->id, index, pivot_index);
  } else {
    append(builder_widget, item_widget);
    std::fprintf(stderr,
                 "[skyline-runtime] %s: #%u index=%d\n",
                 builder->builder_kind == 1 ? "GridBuilderAppendChild"
                                            : "ListBuilderAppendChild",
                 builder->id, index);
  }
  builder->builder_items[index] = Dart_NewPersistentHandle(item_widget);
  builder->builder_item_nodes[index] = item;
  builder->builder_item_signatures[index] = NodeContentSignature(item);
  std::fprintf(stderr,
               "[skyline-runtime] builder mount: #%u index=%d (%gx%g)\n",
               builder->id, index, static_cast<double>(item->measured_w),
               static_cast<double>(item->measured_h));
}

void OnBuilderBuildChild(intptr_t address, int32_t index) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* builder = nullptr;
  auto found = g_builders_by_address.find(address);
  if (found != g_builders_by_address.end()) builder = found->second;
  if (builder == nullptr || builder->builder_items.count(index) != 0 ||
      builder->builder_pending.count(index) != 0) {
    return;
  }
  builder->builder_pending.insert(index);
  RuntimeHolder* holder = g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) {
    builder->builder_pending.erase(index);
    return;
  }
  runner->PostTask([holder, address, index]() {
    DemoPageApiScope scope;
    MaterializeBuilderItem(holder, address, index);
  });
}

void OnBuilderDisposeChild(intptr_t address, int32_t index) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  RuntimeHolder* holder = g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, address, index]() {
    DemoPageApiScope scope;
    auto found = g_builders_by_address.find(address);
    if (found == g_builders_by_address.end() || found->second == nullptr) return;
    AppNode* builder = found->second;
    auto item = builder->builder_items.find(index);
    if (item == builder->builder_items.end()) return;
    auto* remove = reinterpret_cast<BuilderMutateChildFn>(FindCallback(
        holder->ffi_table, builder->builder_kind == 1
                               ? kGridBuilderRemoveChildHash
                               : kListBuilderRemoveChildHash));
    if (remove != nullptr && builder->builder_p != nullptr) {
      remove(Dart_HandleFromPersistent(builder->builder_p),
             Dart_HandleFromPersistent(item->second));
    }
    Dart_DeletePersistentHandle(item->second);
    builder->builder_items.erase(item);
    builder->builder_item_nodes.erase(index);
    builder->builder_item_signatures.erase(index);
    std::fprintf(stderr, "[skyline-runtime] builder dispose: #%u index=%d\n",
                 builder->id, index);
  });
}

// Rebuilds one materialized builder item's content and returns a fresh
// SkylineListBuilderItem/GridBuilderItem widget.  The caller swaps it into the
// builder through ListBuilderUpdateChildren; the item-level callback is also
// invoked for API parity.
Dart_Handle RebuildBuilderItemContent(RuntimeHolder* holder, AppNode* builder,
                                      int index, AppNode* item,
                                      Dart_Handle item_widget) {
  if (holder == nullptr || builder == nullptr || item == nullptr ||
      item_widget == nullptr) {
    return nullptr;
  }
  ResetNodeHandles(item, true);
  PrepareLayoutBuffers(item);
  float width = builder->measured_w > 0.0f
                    ? builder->measured_w
                    : static_cast<float>(g_viewport_width);
  if (width <= 0.0f) width = 320.0f;
  if (builder->builder_kind == 1) {
    const int columns = std::max(1, builder->builder_cross_axis_count);
    width = (width - builder->builder_cross_spacing * (columns - 1)) /
            static_cast<float>(columns);
    if (width < 0.0f) width = 0.0f;
  }
  MeasureNode(item, width, true, kInf, false);
  if (builder->builder_child_height > 0.0f) {
    item->measured_h = builder->builder_child_height;
  }
  if (item->measured_w <= 0.0f) item->measured_w = width;
  std::vector<NodeMetric> metrics;
  LayoutNode(item, 0.0f, 0.0f, item->measured_w, item->measured_h, &metrics);
  WriteLayoutMetrics(metrics);

  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* newText =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newScrollView = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  auto* newListView = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  auto* newGridView = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  auto* newDirectRenderWidget = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  auto* newListBuilder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  auto* newGridBuilder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  auto* update_children = reinterpret_cast<BuilderUpdateChildrenFn>(FindCallback(
      holder->ffi_table, builder->builder_kind == 1
                             ? kGridBuilderItemUpdateChildrenHash
                             : kListBuilderItemUpdateChildrenHash));
  auto* new_item = reinterpret_cast<NewListBuilderItemFn>(FindCallback(
      holder->ffi_table, builder->builder_kind == 1
                             ? kNewGridBuilderItemHash
                             : kNewListBuilderItemHash));
  auto* mark_needs_layout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  if (newYGNode == nullptr || new_item == nullptr) return nullptr;

  Dart_Handle widget =
      BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                  newDecoratedBox, newOpacity, newScrollView, newListView,
                  newGridView, newDirectRenderWidget, newListBuilder,
                  newGridBuilder, true);
  if (widget == nullptr || Dart_IsError(widget)) return nullptr;
  Dart_Handle children = Dart_NewList(1);
  Dart_ListSetAt(children, 0, widget);
  Dart_Handle updated = new_item(children, index);
  if (updated == nullptr || Dart_IsError(updated)) return nullptr;
  // Keep the item-level callback in the path even though Flutter rebuilds the
  // item through the builder's UpdateChildren below (the reconstructed widget
  // does not wire proxySetState for item-local updates).
  if (update_children != nullptr) update_children(updated, children);
  if (mark_needs_layout != nullptr) mark_needs_layout(updated);
  builder->builder_item_signatures[index] = NodeContentSignature(item);
  std::fprintf(stderr,
               "[skyline-runtime] %s: #%u index=%d\n",
               builder->builder_kind == 1 ? "GridBuilderItemUpdateChildren"
                                          : "ListBuilderItemUpdateChildren",
               builder->id, index);
  return updated;
}

// Applies post-attach builder metadata/item changes without rebuilding the
// whole attached subtree.  Returns true when at least one callback was issued.
bool SyncAppServiceBuilders(RuntimeHolder* holder) {
  if (g_page == nullptr || holder == nullptr) return false;
  AppNode* root = FindNode(g_page->root_id);
  if (root != nullptr) ResolveStyles(root, nullptr);
  bool changed = false;
  std::vector<AppNode*> builders;
  builders.reserve(g_builders_by_address.size());
  for (const auto& entry : g_builders_by_address) {
    if (entry.second != nullptr) builders.push_back(entry.second);
  }
  for (AppNode* builder : builders) {
    if (builder == nullptr || builder->released || builder->builder_p == nullptr) {
      continue;
    }
    Dart_Handle builder_widget =
        Dart_HandleFromPersistent(builder->builder_p);
    const bool grid = builder->builder_kind == 1;

    const int child_count = static_cast<int>(
        AttrNumber(builder, "childCount",
                   AttrNumber(builder, "child-count",
                              static_cast<double>(
                                  builder->builder_synced_child_count))));
    if (child_count != builder->builder_synced_child_count) {
      auto* update = reinterpret_cast<BuilderUpdateIntFn>(FindCallback(
          holder->ffi_table, grid ? kGridBuilderUpdateChildCountHash
                                  : kListBuilderUpdateChildCountHash));
      if (update != nullptr) {
        update(builder_widget, child_count);
        builder->builder_child_count = child_count;
        builder->builder_synced_child_count = child_count;
        changed = true;
        std::fprintf(stderr,
                     "[skyline-runtime] %s: #%u childCount=%d\n",
                     grid ? "GridBuilderUpdateChildCount"
                          : "ListBuilderUpdateChildCount",
                     builder->id, child_count);
      }
    }

    if (grid) {
      const int cross = std::max(
          1, static_cast<int>(AttrNumber(builder, "crossAxisCount", 2.0)));
      if (cross != builder->builder_synced_cross_axis_count) {
        auto* update = reinterpret_cast<BuilderUpdateIntFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateCrossAxisCountHash));
        if (update != nullptr) {
          update(builder_widget, cross);
          builder->builder_cross_axis_count = cross;
          builder->builder_synced_cross_axis_count = cross;
          changed = true;
          std::fprintf(stderr,
                       "[skyline-runtime] GridBuilderUpdateCrossAxisCount: "
                       "#%u count=%d\n",
                       builder->id, cross);
        }
      }
      const float main_spacing =
          static_cast<float>(AttrNumber(builder, "mainAxisSpacing", 0.0));
      if (std::fabs(main_spacing - builder->builder_synced_main_spacing) > 0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateMainAxisSpacingHash));
        if (update != nullptr) {
          update(builder_widget, main_spacing);
          builder->builder_main_spacing = main_spacing;
          builder->builder_synced_main_spacing = main_spacing;
          changed = true;
        }
      }
      const float cross_spacing =
          static_cast<float>(AttrNumber(builder, "crossAxisSpacing", 0.0));
      if (std::fabs(cross_spacing - builder->builder_synced_cross_spacing) >
          0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateCrossAxisSpacingHash));
        if (update != nullptr) {
          update(builder_widget, cross_spacing);
          builder->builder_cross_spacing = cross_spacing;
          builder->builder_synced_cross_spacing = cross_spacing;
          changed = true;
        }
      }
      const float max_extent = static_cast<float>(
          AttrNumber(builder, "maxCrossAxisExtent", 0.0));
      if (std::fabs(max_extent - builder->builder_synced_max_cross_extent) >
          0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateMaxCrossAxisExtentHash));
        if (update != nullptr) {
          update(builder_widget, max_extent);
          builder->builder_max_cross_extent = max_extent;
          builder->builder_synced_max_cross_extent = max_extent;
          changed = true;
        }
      }
    } else {
      const float child_height =
          static_cast<float>(AttrNumber(builder, "childHeight", 0.0));
      if (std::fabs(child_height - builder->builder_synced_child_height) >
          0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kListBuilderUpdateChildHeightHash));
        if (update != nullptr) {
          update(builder_widget, child_height);
          builder->builder_child_height = child_height;
          builder->builder_synced_child_height = child_height;
          changed = true;
          std::fprintf(stderr,
                       "[skyline-runtime] ListBuilderUpdateChildHeight: "
                       "#%u height=%g\n",
                       builder->id, child_height);
        }
      }
    }

    const uint32_t background =
        builder->style.has_background ? builder->style.background : 0u;
    if (background != builder->builder_synced_background) {
      auto* update = reinterpret_cast<BuilderUpdateColorFn>(FindCallback(
          holder->ffi_table, grid ? kGridBuilderUpdateBackgroundColorHash
                                  : kListBuilderUpdateBackgroundColorHash));
      if (update != nullptr) {
        update(builder_widget, background);
        builder->builder_background = background;
        builder->builder_synced_background = background;
        changed = true;
      }
    }

    // Item content/index changes.  Metadata updates above keep Flutter's
    // sliver window in sync; content updates use the item-level callbacks.
    std::vector<int> indexes;
    indexes.reserve(builder->builder_items.size());
    for (const auto& entry : builder->builder_items) {
      indexes.push_back(entry.first);
    }
    std::sort(indexes.begin(), indexes.end());
    int content_updates = 0;
    std::vector<int> removed;
    for (int index : indexes) {
      auto item_handle = builder->builder_items.find(index);
      if (item_handle == builder->builder_items.end()) continue;
      AppNode* item = nullptr;
      auto node_entry = builder->builder_item_nodes.find(index);
      if (node_entry != builder->builder_item_nodes.end()) item = node_entry->second;
      if (item == nullptr || item->released) {
        item = FindBuilderItemNode(builder, index);
        if (item == nullptr) {
          removed.push_back(index);
          continue;
        }
        builder->builder_item_nodes[index] = item;
      }
      const int declared_index = static_cast<int>(
          AttrNumber(item, "index", static_cast<double>(index)));
      if (declared_index != index) {
        auto* update_index = reinterpret_cast<BuilderUpdateIntFn>(FindCallback(
            holder->ffi_table, grid ? kGridBuilderItemUpdateIndexHash
                                    : kListBuilderItemUpdateIndexHash));
        if (update_index != nullptr) {
          update_index(Dart_HandleFromPersistent(item_handle->second),
                       declared_index);
          std::fprintf(stderr,
                       "[skyline-runtime] %s: #%u index %d -> %d\n",
                       grid ? "GridBuilderItemUpdateIndex"
                            : "ListBuilderItemUpdateIndex",
                       builder->id, index, declared_index);
        }
      }
      const uint64_t signature = NodeContentSignature(item);
      auto signature_entry = builder->builder_item_signatures.find(index);
      if (signature_entry == builder->builder_item_signatures.end() ||
          signature_entry->second != signature) {
        Dart_Handle updated = RebuildBuilderItemContent(
            holder, builder, index, item,
            Dart_HandleFromPersistent(item_handle->second));
        if (updated != nullptr) {
          Dart_DeletePersistentHandle(item_handle->second);
          builder->builder_items[index] = Dart_NewPersistentHandle(updated);
          content_updates++;
          changed = true;
        }
      }
    }
    for (int index : removed) {
      auto item = builder->builder_items.find(index);
      if (item == builder->builder_items.end()) continue;
      auto* remove = reinterpret_cast<BuilderMutateChildFn>(FindCallback(
          holder->ffi_table, grid ? kGridBuilderRemoveChildHash
                                  : kListBuilderRemoveChildHash));
      if (remove != nullptr) {
        remove(builder_widget, Dart_HandleFromPersistent(item->second));
      }
      Dart_DeletePersistentHandle(item->second);
      builder->builder_items.erase(item);
      builder->builder_item_nodes.erase(index);
      builder->builder_item_signatures.erase(index);
      changed = true;
      std::fprintf(stderr,
                   "[skyline-runtime] %s: #%u remove index=%d\n",
                   grid ? "GridBuilderRemoveChild" : "ListBuilderRemoveChild",
                   builder->id, index);
    }
    if (content_updates > 0) {
      // Hand Flutter one consistent item list; this is the callback that makes
      // the rebuilt item widgets visible to the sliver.
      auto* update_children = reinterpret_cast<BuilderUpdateChildrenFn>(
          FindCallback(holder->ffi_table, grid
                                                ? kGridBuilderUpdateChildrenHash
                                                : kListBuilderUpdateChildrenHash));
      if (update_children != nullptr) {
        std::vector<Dart_Handle> handles;
        handles.reserve(builder->builder_items.size());
        for (int index : indexes) {
          auto item = builder->builder_items.find(index);
          if (item == builder->builder_items.end()) continue;
          handles.push_back(Dart_HandleFromPersistent(item->second));
        }
        Dart_Handle list =
            Dart_NewList(static_cast<intptr_t>(handles.size()));
        for (size_t i = 0; i < handles.size(); i++) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), handles[i]);
        }
        update_children(builder_widget, list);
        std::fprintf(stderr,
                     "[skyline-runtime] %s: #%u (%d item(s), %zu updated)\n",
                     grid ? "GridBuilderUpdateChildren"
                          : "ListBuilderUpdateChildren",
                     builder->id, static_cast<int>(handles.size()),
                     static_cast<size_t>(content_updates));
      }
    }
  }
  return changed;
}


uint32_t MemberUint(const rapidjson::Value& object, const char* key,
                    uint32_t fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsUint()) return fallback;
  return member->value.GetUint();
}

const char* MemberText(const rapidjson::Value& object, const char* key,
                       const char* fallback = "") {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsString()) return fallback;
  return member->value.GetString();
}

void ApplyNodeAttribute(AppNode* node, const char* name,
                        const rapidjson::Value& value) {
  if (value.IsString()) {
    ApplyAttribute(node, name, value.GetString());
  } else if (value.IsNumber()) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value.GetDouble());
    ApplyAttribute(node, name, buffer);
  } else if (value.IsBool()) {
    ApplyAttribute(node, name, value.GetBool() ? "1" : "0");
  }
}

std::string CamelToKebab(const std::string& key) {
  std::string out;
  out.reserve(key.size() + 4);
  for (char c : key) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back('-');
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool IsColorDeclaration(const std::string& key) {
  return key == "background-color" || key == "color" ||
         key == "border-color" || EndsWith(key, "-color");
}

bool IsLengthDeclaration(const std::string& key) {
  return key == "width" || key == "height" || key == "left" || key == "top" ||
         key == "right" || key == "bottom" || key == "font-size" ||
         key == "border-width" || key == "border-radius";
}

// Converts a React-Native-style animated style map into CSS declarations.
std::string AnimatedStyleDeclarations(const rapidjson::Value& style) {
  std::string out;
  if (!style.IsObject()) return out;
  for (auto it = style.MemberBegin(); it != style.MemberEnd(); ++it) {
    const std::string key = CamelToKebab(it->name.GetString());
    const rapidjson::Value& value = it->value;
    out += key;
    out.push_back(':');
    if (value.IsString()) {
      out += value.GetString();
    } else if (value.IsBool()) {
      out += value.GetBool() ? "1" : "0";
    } else if (value.IsNumber()) {
      char buffer[64];
      if (IsColorDeclaration(key)) {
        std::snprintf(buffer, sizeof(buffer), "#%08X",
                      static_cast<unsigned>(value.GetUint()));
      } else if (IsLengthDeclaration(key) && value.GetDouble() != 0.0) {
        std::snprintf(buffer, sizeof(buffer), "%gpx", value.GetDouble());
      } else {
        std::snprintf(buffer, sizeof(buffer), "%g", value.GetDouble());
      }
      out += buffer;
    } else if (value.IsArray() && key == "transform") {
      bool first = true;
      for (const rapidjson::Value& entry : value.GetArray()) {
        if (!entry.IsObject()) continue;
        for (auto fn = entry.MemberBegin(); fn != entry.MemberEnd(); ++fn) {
          const std::string name = fn->name.GetString();
          const rapidjson::Value& arg = fn->value;
          if (!first) out.push_back(' ');
          first = false;
          out += name;
          out.push_back('(');
          if (arg.IsString()) {
            out += arg.GetString();
          } else if (arg.IsNumber()) {
            char buffer[64];
            const bool angle = StartsWith(name, "rotate") ||
                               StartsWith(name, "skew");
            const bool scale = StartsWith(name, "scale");
            if (angle) {
              std::snprintf(buffer, sizeof(buffer), "%gdeg", arg.GetDouble());
            } else if (scale) {
              std::snprintf(buffer, sizeof(buffer), "%g", arg.GetDouble());
            } else {
              std::snprintf(buffer, sizeof(buffer), "%gpx", arg.GetDouble());
            }
            out += buffer;
          }
          out.push_back(')');
        }
      }
    } else {
      out += "0";
    }
    out.push_back(';');
  }
  return out;
}

void CollectAnimatedStyles(const rapidjson::Value& op, AppNode* node) {
  if (g_page == nullptr || node == nullptr) return;
  const auto timeline = op.FindMember("timeline");
  if (timeline != op.MemberEnd() && timeline->value.IsArray()) {
    for (const rapidjson::Value& frame : timeline->value.GetArray()) {
      if (!frame.IsObject()) continue;
      const auto style = frame.FindMember("style");
      if (style == frame.MemberEnd()) continue;
      PendingAnimatedStyle entry;
      entry.node_id = node->id;
      entry.at_ms = static_cast<int>(MemberNumber(frame, "atMs", 0.0));
      entry.declarations = AnimatedStyleDeclarations(style->value);
      if (!entry.declarations.empty()) {
        g_page->animated_styles.push_back(entry);
      }
    }
  }
  const auto updates = op.FindMember("updates");
  const auto style = op.FindMember("style");
  const rapidjson::Value* value = nullptr;
  if (updates != op.MemberEnd()) value = &updates->value;
  else if (style != op.MemberEnd()) value = &style->value;
  if (value != nullptr) {
    PendingAnimatedStyle entry;
    entry.node_id = node->id;
    entry.at_ms = static_cast<int>(MemberNumber(op, "atMs", 0.0));
    entry.declarations = AnimatedStyleDeclarations(*value);
    if (!entry.declarations.empty()) {
      g_page->animated_styles.push_back(entry);
    }
  }
  std::fprintf(stderr,
               "[skyline-runtime] addAnimatedStyle: #%u (%zu timeline frame(s))\n",
               node->id, g_page->animated_styles.size());
}

// -- synthetic input state ---------------------------------------------------

int Utf8CodepointCount(const std::string& text) {
  int count = 0;
  size_t index = 0;
  while (index < text.size()) {
    NextCodepoint(text, &index);
    count++;
  }
  return count;
}

size_t Utf8ByteOffset(const std::string& text, int codepoint_index) {
  if (codepoint_index <= 0) return 0;
  size_t index = 0;
  int count = 0;
  while (index < text.size() && count < codepoint_index) {
    NextCodepoint(text, &index);
    count++;
  }
  return index;
}

void Utf8Erase(std::string* text, int start, int end) {
  if (text == nullptr || start >= end) return;
  const size_t begin = Utf8ByteOffset(*text, start);
  const size_t finish = Utf8ByteOffset(*text, end);
  if (begin < finish) text->erase(begin, finish - begin);
}

void Utf8Insert(std::string* text, int position, const std::string& value) {
  if (text == nullptr || value.empty()) return;
  const size_t offset = Utf8ByteOffset(*text, position);
  text->insert(offset, value);
}

void ClampInputState(AppNode* node) {
  if (node == nullptr) return;
  const int length = Utf8CodepointCount(node->text);
  auto clamp = [length](int value) {
    if (value < 0) return 0;
    if (value > length) return length;
    return value;
  };
  node->input_cursor = clamp(node->input_cursor);
  node->input_selection_start = clamp(node->input_selection_start);
  node->input_selection_end = clamp(node->input_selection_end);
}

void CollapseInputSelection(AppNode* node) {
  node->input_selection_start = node->input_cursor;
  node->input_selection_end = node->input_cursor;
}

// Replaces the current selection (or inserts at the cursor) and collapses the
// selection after the inserted text.
void InsertInputText(AppNode* node, const std::string& value) {
  if (node == nullptr) return;
  ClampInputState(node);
  int start = node->input_selection_start;
  int end = node->input_selection_end;
  if (start > end) std::swap(start, end);
  if (start == end) {
    start = end = node->input_cursor;
  }
  Utf8Erase(&node->text, start, end);
  Utf8Insert(&node->text, start, value);
  node->input_cursor = start + Utf8CodepointCount(value);
  node->composing = false;
  node->composition_base_text.clear();
  CollapseInputSelection(node);
}

// IME composition preview: the uncommitted text replaces the marked range of
// the original value until compositionEnd commits it.
void ReplaceComposition(AppNode* node, const std::string& value) {
  if (node == nullptr) return;
  std::string text = node->composition_base_text;
  Utf8Erase(&text, node->composition_start, node->composition_end);
  Utf8Insert(&text, node->composition_start, value);
  node->text = text;
  node->composition_text = value;
  node->input_cursor =
      node->composition_start + Utf8CodepointCount(value);
  node->input_selection_start = node->composition_start;
  node->input_selection_end = node->input_cursor;
}

// Mirrors the state to a real SkylineInput when one was materialized.
void NotifyInputState(AppNode* node) {
  if (node == nullptr || node->input_widget_p == nullptr || g_page == nullptr ||
      g_page->holder == nullptr) {
    if (node != nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] InputHoldKeyboard: id=%u hold=%d "
                   "(no native input widget)\n",
                   node->id, node->input_hold_keyboard ? 1 : 0);
    }
    return;
  }
  RuntimeHolder* holder = g_page->holder;
  Dart_Handle widget = Dart_HandleFromPersistent(node->input_widget_p);
  auto* value = reinterpret_cast<InputValueFn>(
      FindCallback(holder->ffi_table, kInputValueHash));
  auto* cursor = reinterpret_cast<InputIntFn>(
      FindCallback(holder->ffi_table, kInputCursorHash));
  auto* selection_start = reinterpret_cast<InputIntFn>(
      FindCallback(holder->ffi_table, kInputSelectionStartHash));
  auto* selection_end = reinterpret_cast<InputIntFn>(
      FindCallback(holder->ffi_table, kInputSelectionEndHash));
  auto* hold = reinterpret_cast<InputBoolFn>(
      FindCallback(holder->ffi_table, kInputHoldKeyboardHash));
  if (value != nullptr) {
    Dart_Handle text = Dart_NewStringFromCString(node->text.c_str());
    if (!Dart_IsError(text)) value(widget, text);
  }
  if (cursor != nullptr) cursor(widget, node->input_cursor);
  if (selection_start != nullptr) {
    selection_start(widget, node->input_selection_start);
  }
  if (selection_end != nullptr) selection_end(widget, node->input_selection_end);
  if (hold != nullptr) {
    hold(widget, node->input_hold_keyboard ? 1 : 0);
    std::fprintf(stderr,
                 "[skyline-runtime] InputHoldKeyboard: id=%u hold=%d\n",
                 node->id, node->input_hold_keyboard ? 1 : 0);
  }
}


void ApplyOperation(const rapidjson::Value& op) {
  if (g_page == nullptr || !op.IsObject()) return;
  const char* kind = MemberText(op, "op");
  if (kind[0] == '\0') return;
  g_page->applied_ops++;

  if (std::strcmp(kind, "focusInput") == 0) {
    auto op_int = [&](const char* key, int fallback) -> int {
      const auto member = op.FindMember(key);
      if (member == op.MemberEnd()) return fallback;
      if (member->value.IsInt()) return member->value.GetInt();
      if (member->value.IsUint()) {
        return static_cast<int>(member->value.GetUint());
      }
      if (member->value.IsDouble()) {
        return static_cast<int>(member->value.GetDouble());
      }
      if (member->value.IsBool()) return member->value.GetBool() ? 1 : 0;
      return fallback;
    };
    const uint32_t id = MemberUint(op, "id", 0);
    g_page->focused_input_id = id;
    AppNode* node = FindNode(id);
    if (node != nullptr) {
      node->input_hold_keyboard = op_int("holdKeyboard",
                                         node->input_hold_keyboard ? 1 : 0) != 0;
      const bool has_cursor = op.FindMember("cursor") != op.MemberEnd();
      const int cursor =
          op_int("cursor", has_cursor ? 0 : Utf8CodepointCount(node->text));
      const int start = op_int("selectionStart", cursor);
      const int end = op_int("selectionEnd", cursor);
      node->input_cursor = cursor;
      node->input_selection_start = start;
      node->input_selection_end = end;
      ClampInputState(node);
      NotifyInputState(node);
    }
    std::fprintf(stderr, "[skyline-runtime] focus input: id=%u\n",
                 g_page->focused_input_id);
  } else if (std::strcmp(kind, "setSelection") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const int cursor = static_cast<int>(MemberNumber(op, "cursor", -1.0));
      const int start = static_cast<int>(
          MemberNumber(op, "start",
                       cursor >= 0 ? cursor : node->input_selection_start));
      const int end = static_cast<int>(
          MemberNumber(op, "end", cursor >= 0 ? cursor : node->input_selection_end));
      node->input_cursor = cursor >= 0 ? cursor : start;
      node->input_selection_start = start;
      node->input_selection_end = end;
      ClampInputState(node);
      NotifyInputState(node);
      std::fprintf(stderr,
                   "[skyline-runtime] setSelection: id=%u selection=[%d,%d] "
                   "cursor=%d\n",
                   node->id, node->input_selection_start,
                   node->input_selection_end, node->input_cursor);
    }
  } else if (std::strcmp(kind, "compositionStart") == 0 ||
             std::strcmp(kind, "compositionUpdate") == 0 ||
             std::strcmp(kind, "compositionEnd") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const bool start = std::strcmp(kind, "compositionStart") == 0;
      const bool end = std::strcmp(kind, "compositionEnd") == 0;
      if (start) {
        node->composing = true;
        node->composition_base_text = node->text;
        int range_start = node->input_selection_start;
        int range_end = node->input_selection_end;
        if (range_start > range_end) std::swap(range_start, range_end);
        if (range_start == range_end) range_start = range_end = node->input_cursor;
        const auto start_member = op.FindMember("start");
        const auto end_member = op.FindMember("end");
        if (start_member != op.MemberEnd() && start_member->value.IsInt()) {
          range_start = start_member->value.GetInt();
        }
        if (end_member != op.MemberEnd() && end_member->value.IsInt()) {
          range_end = end_member->value.GetInt();
        }
        node->composition_start = range_start;
        node->composition_end = range_end;
      }
      if (start || node->composing) {
        const std::string value = MemberText(op, "text");
        ReplaceComposition(node, value);
        if (end) {
          node->composing = false;
          node->composition_base_text.clear();
          node->input_cursor = node->composition_start +
                               Utf8CodepointCount(node->composition_text);
          CollapseInputSelection(node);
        }
        NotifyInputState(node);
        std::fprintf(stderr,
                     "[skyline-runtime] %s: id=%u text=\"%s\" cursor=%d "
                     "selection=[%d,%d] value=\"%s\"\n",
                     start ? "composition start"
                           : (end ? "composition end" : "composition update"),
                     node->id, value.c_str(), node->input_cursor,
                     node->input_selection_start, node->input_selection_end,
                     node->text.c_str());
      }
    }
  } else if (std::strcmp(kind, "dispatchKey") == 0) {
    AppNode* input = FindNode(g_page->focused_input_id);
    const char* text = MemberText(op, "text");
    if (input != nullptr && text[0] != '\0') {
      InsertInputText(input, text);
      NotifyInputState(input);
      std::fprintf(stderr,
                   "[skyline-runtime] dispatchKey: id=%u text=%s cursor=%d\n",
                   input->id, input->text.c_str(), input->input_cursor);
    }
  } else if (std::strcmp(kind, "keyboardStatus") == 0) {
    g_page->keyboard_height = MemberNumber(op, "height", 0.0);
    const int32_t input_key = static_cast<int32_t>(MemberUint(op, "inputKey", 0));
    void* callback = g_page->holder != nullptr
                         ? FindCallback(g_page->holder->ffi_table,
                                        kUpdateKeyboardStatusHash)
                         : nullptr;
    if (callback != nullptr) {
      reinterpret_cast<UpdateKeyboardStatusFn>(callback)(
          input_key, g_page->keyboard_height);
      std::fprintf(stderr,
                   "[skyline-runtime] UpdateKeyboardStatus: key=%d height=%g\n",
                   input_key, g_page->keyboard_height);
    }
  } else if (std::strcmp(kind, "setScrollTop") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    void* callback = g_page->holder != nullptr
                         ? FindCallback(g_page->holder->ffi_table,
                                        kScrollViewUpdateScrollTopHash)
                         : nullptr;
    if (node != nullptr && callback != nullptr &&
        node->scroll_view_p != nullptr) {
      const double value = MemberNumber(op, "value", 0.0);
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%g", value);
      node->attrs["scrollTop"] = buffer;
      reinterpret_cast<ScrollViewUpdateScrollTopFn>(callback)(
          Dart_HandleFromPersistent(node->scroll_view_p), value);
      std::fprintf(stderr,
                   "[skyline-runtime] appservice setScrollTop: id=%u value=%g\n",
                   node->id, value);
    }
  } else if (std::strcmp(kind, "dispatchTouch") == 0) {
    void* callback = g_page->holder != nullptr
                         ? FindCallback(g_page->holder->ffi_table,
                                        kDispatchTouchEventHash)
                         : nullptr;
    if (callback != nullptr) {
      const double x = MemberNumber(op, "x", 0.0);
      const double y = MemberNumber(op, "y", 0.0);
      const uint8_t type =
          static_cast<uint8_t>(MemberUint(op, "type", 0));
      reinterpret_cast<DispatchTouchEventFn>(callback)(type, x, y);
      if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        using GetNodeFromPointFn = intptr_t (*)(double, double, uint8_t);
        auto* hit = reinterpret_cast<GetNodeFromPointFn>(
            FindCallback(g_page->holder->ffi_table, kGetNodeFromPointHash));
        const intptr_t address = hit != nullptr
                                     ? hit(x, y, 1)
                                     : static_cast<intptr_t>(0);
        std::fprintf(stderr,
                     "[skyline-runtime] appservice dispatchTouch: type=%u (%g,%g) "
                     "hit=0x%lx\n",
                     static_cast<unsigned>(type), x, y,
                     static_cast<long>(address));
      } else {
        std::fprintf(stderr,
                     "[skyline-runtime] appservice dispatchTouch: type=%u (%g,%g)\n",
                     static_cast<unsigned>(type), x, y);
      }
    }
  } else if (std::strcmp(kind, "createElement") == 0) {
    NewNode(MemberUint(op, "id", 0), MemberText(op, "tag"),
            MemberText(op, "name"));
  } else if (std::strcmp(kind, "notifyNavigateTo") == 0 ||
             std::strcmp(kind, "notifySwitchTab") == 0 ||
             std::strcmp(kind, "notifyNavigateBack") == 0) {
    // Route requests from the renderer.  The recorder stores the native call
    // arguments as an array (`[windowId, pageId, ...]`).
    const auto args = op.FindMember("args");
    auto arg_number = [&](size_t index, double fallback) -> double {
      if (args == op.MemberEnd() || !args->value.IsArray() ||
          index >= args->value.Size()) {
        return fallback;
      }
      const rapidjson::Value& value = args->value[static_cast<rapidjson::SizeType>(
          index)];
      if (value.IsNumber()) return value.GetDouble();
      return fallback;
    };
    const bool is_back = std::strcmp(kind, "notifyNavigateBack") == 0;
    const bool is_switch = std::strcmp(kind, "notifySwitchTab") == 0;
    g_page->pending_open_type = is_back ? 6 : (is_switch ? 1 : 4);
    g_page->pending_page_id =
        static_cast<int32_t>(arg_number(1, 0.0));
    if (is_back) {
      g_page->pending_back_delta =
          static_cast<int>(arg_number(2, 1.0));
      g_page->pending_back_animated = arg_number(3, 1.0) != 0.0;
    }
  } else if (std::strcmp(kind, "createContext") == 0 ||
             std::strcmp(kind, "setAsTab") == 0 ||
             std::strcmp(kind, "registerEventCallback") == 0 ||
             std::strcmp(kind, "registerAnimationCallback") == 0 ||
             std::strcmp(kind, "startMapper") == 0 ||
             std::strcmp(kind, "stopMapper") == 0) {
    // Native bindings with no page-tree effect in this milestone.
  } else if (std::strcmp(kind, "appendStyleSheet") == 0) {
    const auto member = op.FindMember("css");
    if (member != op.MemberEnd() && member->value.IsString()) {
      ParseStyleSheet(member->value.GetString());
    }
  } else if (std::strcmp(kind, "createElementDone") == 0) {
    // Reserved.
  } else if (std::strcmp(kind, "setText") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) node->text = MemberText(op, "text");
  } else if (std::strcmp(kind, "setAttribute") == 0 ||
             std::strcmp(kind, "setAttributeValue") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    const auto value = op.FindMember("value");
    if (node != nullptr && value != op.MemberEnd()) {
      const char* attr = MemberText(op, "name");
      ApplyNodeAttribute(node, attr, value->value);
      if (IsImageName(node->name) &&
          (std::strcmp(attr, "src") == 0 || std::strcmp(attr, "url") == 0 ||
           std::strcmp(attr, "imageUrl") == 0 ||
           std::strcmp(attr, "image") == 0 ||
           std::strcmp(attr, "path") == 0)) {
        g_page->force_rebuild = true;
      }
    }
  } else if (std::strcmp(kind, "setAttributes") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    const auto attrs = op.FindMember("attrs");
    if (node != nullptr && attrs != op.MemberEnd() &&
        attrs->value.IsObject()) {
      for (auto it = attrs->value.MemberBegin(); it != attrs->value.MemberEnd();
           ++it) {
        const std::string attr = it->name.GetString();
        ApplyNodeAttribute(node, attr.c_str(), it->value);
        if (IsImageName(node->name) &&
            (attr == "src" || attr == "url" || attr == "imageUrl" ||
             attr == "image" || attr == "path")) {
          g_page->force_rebuild = true;
        }
      }
    }
  } else if (std::strcmp(kind, "removeAttribute") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      node->attrs.erase(MemberText(op, "name"));
    }
  } else if (std::strcmp(kind, "setStyle") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) ApplyAttribute(node, "style", MemberText(op, "value"));
  } else if (std::strcmp(kind, "addClass") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const std::string klass = MemberText(op, "prefix");
      if (!klass.empty() && !NodeHasClass(node, klass)) {
        node->classes.push_back(klass);
      }
    }
  } else if (std::strcmp(kind, "removeClass") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const std::string klass = MemberText(op, "prefix");
      node->classes.erase(
          std::remove(node->classes.begin(), node->classes.end(), klass),
          node->classes.end());
    }
  } else if (std::strcmp(kind, "setClass") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      SetClasses(node, SplitClasses(MemberText(op, "names")));
    }
  } else if (std::strcmp(kind, "setId") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) node->attrs["id"] = MemberText(op, "value");
  } else if (std::strcmp(kind, "appendChild") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    if (parent != nullptr && child != nullptr) {
      if (child->tag == "fragment") {
        std::vector<AppNode*> nested = child->children;
        child->children.clear();
        for (AppNode* item : nested) AppendChild(parent, item);
      } else {
        AppendChild(parent, child);
      }
    }
  } else if (std::strcmp(kind, "insertBefore") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    AppNode* before = FindNode(MemberUint(op, "before", 0));
    if (parent != nullptr && child != nullptr) {
      InsertBefore(parent, child, before);
    }
  } else if (std::strcmp(kind, "removeChild") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    RemoveChild(parent, child);
  } else if (std::strcmp(kind, "replaceChild") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    const auto index_member = op.FindMember("index");
    if (parent != nullptr && child != nullptr) {
      DetachFromParent(child);
      const int index = index_member != op.MemberEnd() &&
                                index_member->value.IsInt()
                            ? index_member->value.GetInt()
                            : static_cast<int>(parent->children.size());
      if (index >= 0 && index < static_cast<int>(parent->children.size())) {
        parent->children[static_cast<size_t>(index)]->parent = nullptr;
        parent->children[static_cast<size_t>(index)] = child;
      } else {
        parent->children.push_back(child);
      }
      child->parent = parent;
    }
  } else if (std::strcmp(kind, "splice") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    const auto items_member = op.FindMember("items");
    std::vector<AppNode*> items;
    if (items_member != op.MemberEnd() && items_member->value.IsArray()) {
      for (const rapidjson::Value& item : items_member->value.GetArray()) {
        if (item.IsUint()) items.push_back(FindNode(item.GetUint()));
      }
    }
    const auto pos_member = op.FindMember("pos");
    const auto count_member = op.FindMember("count");
    const int pos = pos_member != op.MemberEnd() && pos_member->value.IsInt()
                        ? pos_member->value.GetInt()
                        : -1;
    const int count =
        count_member != op.MemberEnd() && count_member->value.IsInt()
            ? count_member->value.GetInt()
            : 0;
    Splice(parent, pos, count, items);
  } else if (std::strcmp(kind, "release") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      CancelPendingImage(node);
      DetachFromParent(node);
      node->released = true;
    }
  } else if (std::strcmp(kind, "addAnimatedStyle") == 0) {
    CollectAnimatedStyles(op, FindNode(MemberUint(op, "id", 0)));
  } else if (std::strcmp(kind, "attach") == 0) {
    g_page->root_id = MemberUint(op, "root", 0);
  } else if (std::strcmp(kind, "startRender") == 0) {
    g_page->started = true;
  } else {
    // setStyleScope / associateComponent / setListenerOption /
    // setHitTestBehavior / forceDetached / ...
  }
}

void DebugDumpNode(const AppNode* node, int depth) {
  if (node == nullptr) return;
  std::fprintf(stderr,
               "[ops-debug] %*s#%u tag=%s name=%s classes=%zu style(w=%g h=%g "
               "pctw=%g pcth=%g pad=%g,%g,%g,%g margin=%g,%g,%g,%g row=%d "
               "justify=%d align=%d bg=%08x border=%g font=%g) measured=%gx%g "
               "address=0x%lx\n",
               depth * 2, "", node->id, node->tag.c_str(), node->name.c_str(),
               node->classes.size(), static_cast<double>(node->style.width),
               static_cast<double>(node->style.height),
               static_cast<double>(node->style.width_percent),
               static_cast<double>(node->style.height_percent),
               static_cast<double>(node->style.padding[0]),
               static_cast<double>(node->style.padding[1]),
               static_cast<double>(node->style.padding[2]),
               static_cast<double>(node->style.padding[3]),
               static_cast<double>(node->style.margin[0]),
               static_cast<double>(node->style.margin[1]),
               static_cast<double>(node->style.margin[2]),
               static_cast<double>(node->style.margin[3]), node->style.row,
               node->style.justify, node->style.align,
               node->style.has_background ? node->style.background : 0u,
               static_cast<double>(node->style.border_width),
               node->style.font_size, static_cast<double>(node->measured_w),
               static_cast<double>(node->measured_h),
               static_cast<long>(node->address));
  for (const AppNode* child : node->children) {
    if (child->released) continue;
    DebugDumpNode(child, depth + 1);
  }
}

// Post-attach updates.
//
// Structural batches (create/append/remove/...) rebuild the root's children and
// swap them through `YGConnectorUpdateChildren`.  Style/text-only batches use
// the fine-grained runtime callbacks (`DecoratedBoxUpdateColor`,
// `DecoratedBoxUpdateBorder`, `OpacityUpdateOpacity`, `TextSpanUpdate*`,
// `TextApplyUpdate`) and re-run the layout pass against the existing buffers.

bool Nearly(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

bool SameFloatOrNan(float a, float b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) && std::isnan(b);
  }
  return std::fabs(a - b) < 0.01f;
}

bool SameGradient(const AppStyle& a, const AppStyle& b) {
  if (a.has_gradient != b.has_gradient) return false;
  if (!a.has_gradient) return true;
  if (std::fabs(a.gradient_angle - b.gradient_angle) > 0.001) return false;
  if (a.gradient_colors != b.gradient_colors) return false;
  if (a.gradient_stops.size() != b.gradient_stops.size()) return false;
  for (size_t i = 0; i < a.gradient_stops.size(); i++) {
    if (std::fabs(a.gradient_stops[i] - b.gradient_stops[i]) > 0.001) {
      return false;
    }
  }
  return true;
}

bool StyleNeedsRebuild(const AppNode* node, const AppStyle& before,
                       const AppStyle& after) {
  if (before.display_none != after.display_none) return true;
  if (before.clip != after.clip) return true;
  // Presence changes can be handled in place once the corresponding widget
  // handles already exist (notably during an opacity/background transition).
  if (node->decorated_box_p == nullptr &&
      before.has_background != after.has_background) {
    return true;
  }
  if (node->decorated_box_p == nullptr &&
      HasBorder(before) != HasBorder(after)) {
    return true;
  }
  if (node->opacity_widget_p == nullptr &&
      (before.opacity < 0.999) != (after.opacity < 0.999)) {
    return true;
  }
  if (node->decorated_box_p == nullptr && before.has_shadow != after.has_shadow) {
    return true;
  }
  if (node->decorated_box_p == nullptr &&
      before.has_gradient != after.has_gradient) {
    return true;
  }
  if (node->decorated_box_p == nullptr && RadiiChanged(before, after)) {
    return true;
  }
  return false;
}

void SnapshotNode(AppNode* node) {
  if (node == nullptr) return;
  node->previous_style = node->style;
  node->has_previous_style = true;
  if (node->tag == "text") {
    std::string storage;
    node->previous_text = NodeText(node, &storage);
  }
  for (AppNode* child : node->children) SnapshotNode(child);
}

bool IsStructuralOpKind(const char* kind) {
  return std::strcmp(kind, "createElement") == 0 ||
         std::strcmp(kind, "appendChild") == 0 ||
         std::strcmp(kind, "insertBefore") == 0 ||
         std::strcmp(kind, "removeChild") == 0 ||
         std::strcmp(kind, "replaceChild") == 0 ||
         std::strcmp(kind, "splice") == 0 ||
         std::strcmp(kind, "setScrollTop") == 0 ||
         std::strcmp(kind, "release") == 0;
}

struct TextUpdateCallbacks {
  TextSpanUpdateTextFn text = nullptr;
  TextSpanUpdateTextColorFn color = nullptr;
  TextSpanUpdateFontSizeFn font_size = nullptr;
  TextSpanUpdateFontWeightFn font_weight = nullptr;
  TextSpanUpdateFontStyleFn font_style = nullptr;
  TextSpanUpdateLineHeightFn line_height = nullptr;
  TextSpanUpdateLetterSpacingFn letter_spacing = nullptr;
  TextApplyUpdateFn apply = nullptr;
  TextUpdateTextAlignFn align = nullptr;
  TextUpdateMaxLinesFn max_lines = nullptr;
};

bool ApplyTargetedUpdates(RuntimeHolder* holder) {
  if (g_page == nullptr || g_page->root_node == nullptr) return false;
  AppNode* root = FindNode(g_page->root_id);
  if (root == nullptr) return false;

  auto* update_color = reinterpret_cast<DecoratedBoxUpdateColorFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateColorHash));
  auto* update_border = reinterpret_cast<DecoratedBoxUpdateBorderFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateBorderHash));
  auto* update_shadow = reinterpret_cast<DecoratedBoxUpdateShadowFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateShadowHash));
  auto* update_radius = reinterpret_cast<DecoratedBoxUpdateRadiusFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateBorderSideRadiusHash));
  auto* update_gradient = reinterpret_cast<DecoratedBoxUpdateGradientFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateGradientHash));
  auto* clear_gradient = reinterpret_cast<DecoratedBoxClearGradientFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxClearGradientHash));
  auto* update_clip_radius = reinterpret_cast<ClipRRectUpdateRadiusFn>(
      FindCallback(holder->ffi_table, kClipRRectUpdateRadiusHash));
  auto* update_opacity = reinterpret_cast<OpacityUpdateFn>(
      FindCallback(holder->ffi_table, kOpacityUpdateHash));
  auto* mark_needs_layout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  TextUpdateCallbacks text;
  text.text = reinterpret_cast<TextSpanUpdateTextFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateTextHash));
  text.color = reinterpret_cast<TextSpanUpdateTextColorFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateTextColorHash));
  text.font_size = reinterpret_cast<TextSpanUpdateFontSizeFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateFontSizeHash));
  text.font_weight = reinterpret_cast<TextSpanUpdateFontWeightFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateFontWeightHash));
  text.font_style = reinterpret_cast<TextSpanUpdateFontStyleFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateFontStyleHash));
  text.line_height = reinterpret_cast<TextSpanUpdateLineHeightFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateLineHeightHash));
  text.letter_spacing = reinterpret_cast<TextSpanUpdateLetterSpacingFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateLetterSpacingHash));
  text.apply = reinterpret_cast<TextApplyUpdateFn>(
      FindCallback(holder->ffi_table, kTextApplyUpdateHash));
  text.align = reinterpret_cast<TextUpdateTextAlignFn>(
      FindCallback(holder->ffi_table, kTextUpdateTextAlignHash));
  text.max_lines = reinterpret_cast<TextUpdateMaxLinesFn>(
      FindCallback(holder->ffi_table, kTextUpdateMaxLinesHash));
  if (mark_needs_layout == nullptr) return false;

  ResolveStyles(root, nullptr);

  std::vector<Dart_Handle> yoga_nodes;
  bool rebuild = false;

  std::vector<AppNode*> pending;
  pending.push_back(root);
  while (!pending.empty() && !rebuild) {
    AppNode* node = pending.back();
    pending.pop_back();
    for (AppNode* child : node->children) {
      if (!child->released) pending.push_back(child);
    }
    if (!node->has_previous_style) {
      rebuild = true;
      break;
    }
    if (StyleNeedsRebuild(node, node->previous_style, node->style)) {
      rebuild = true;
      break;
    }
    const AppStyle& before = node->previous_style;
    const AppStyle& after = node->style;

    if (node->tag == "text") {
      std::string storage;
      const std::string text_value = NodeText(node, &storage);
      const bool text_changed = text_value != node->previous_text;
      const bool color_changed = before.color != after.color;
      const bool size_changed = std::fabs(before.font_size - after.font_size) > 0.01;
      const bool weight_changed = before.font_weight != after.font_weight;
      const bool style_changed = before.font_style != after.font_style;
      const double before_height =
          before.line_height > 0 ? before.line_height / before.font_size
                                 : before.line_height_factor;
      const double after_height =
          after.line_height > 0 ? after.line_height / after.font_size
                                : after.line_height_factor;
      const bool height_changed = std::fabs(before_height - after_height) > 0.01;
      const bool spacing_changed =
          (std::isnan(before.letter_spacing) != std::isnan(after.letter_spacing)) ||
          (!std::isnan(before.letter_spacing) &&
           !std::isnan(after.letter_spacing) &&
           std::fabs(before.letter_spacing - after.letter_spacing) > 0.01);
      const bool align_changed = before.text_align != after.text_align;
      const bool max_lines_changed = before.max_lines != after.max_lines;
      const bool any = text_changed || color_changed || size_changed ||
                       weight_changed || style_changed || height_changed ||
                       spacing_changed || align_changed || max_lines_changed;
      if (any) {
        if (node->text_span_p == nullptr || node->text_widget_p == nullptr ||
            text.text == nullptr || text.apply == nullptr) {
          rebuild = true;
          break;
        }
        Dart_Handle span = Dart_HandleFromPersistent(node->text_span_p);
        Dart_Handle widget = Dart_HandleFromPersistent(node->text_widget_p);
        if (text_changed) {
          Dart_Handle value = Dart_NewStringFromCString(text_value.c_str());
          if (!Dart_IsError(value)) text.text(span, value);
        }
        if (color_changed && text.color != nullptr) text.color(span, after.color);
        if (size_changed && text.font_size != nullptr) {
          text.font_size(span, after.font_size);
        }
        if (weight_changed && text.font_weight != nullptr) {
          text.font_weight(span, static_cast<uint8_t>(after.font_weight));
        }
        if (style_changed && text.font_style != nullptr) {
          text.font_style(span, static_cast<uint8_t>(after.font_style));
        }
        if (height_changed && text.line_height != nullptr) {
          text.line_height(span, after_height);
        }
        if (spacing_changed && text.letter_spacing != nullptr) {
          text.letter_spacing(span, std::isnan(after.letter_spacing)
                                        ? std::nan("")
                                        : after.letter_spacing);
        }
        if (align_changed && text.align != nullptr) {
          text.align(widget, static_cast<uint8_t>(after.text_align));
        }
        if (max_lines_changed && text.max_lines != nullptr) {
          text.max_lines(widget, after.max_lines);
        }
        text.apply(widget, 1);
      }
    } else {
      if (node->decorated_box_p != nullptr) {
        Dart_Handle decorated =
            Dart_HandleFromPersistent(node->decorated_box_p);
        if (after.has_background && before.background != after.background &&
            update_color != nullptr) {
          update_color(decorated, after.background);
        }
        if (BorderSidesChanged(before, after) && update_border != nullptr) {
          double widths[4] = {0, 0, 0, 0};
          uint32_t colors[4] = {0u, 0u, 0u, 0u};
          uint8_t styles[4] = {0, 0, 0, 0};
          for (int side = 0; side < 4; side++) {
            widths[side] = EffectiveBorderWidth(after, side);
            if (widths[side] > 0.0) {
              colors[side] = EffectiveBorderColor(after, side);
              styles[side] = 1;
            }
          }
          update_border(decorated, widths[3], widths[0], widths[1], widths[2],
                        colors[3], colors[0], colors[1], colors[2], styles[3],
                        styles[0], styles[1], styles[2]);
        }
        const bool shadow_changed =
            before.has_shadow != after.has_shadow ||
            before.shadow_color != after.shadow_color ||
            !Nearly(before.shadow_offset_x, after.shadow_offset_x) ||
            !Nearly(before.shadow_offset_y, after.shadow_offset_y) ||
            !Nearly(before.shadow_blur, after.shadow_blur) ||
            !Nearly(before.shadow_spread, after.shadow_spread) ||
            before.shadow_inset != after.shadow_inset;
        if (shadow_changed && update_shadow != nullptr) {
          update_shadow(decorated, BuildBoxShadowList(after));
        }
        if (RadiiChanged(before, after)) {
          if (update_radius == nullptr) {
            rebuild = true;
            break;
          }
          static const uint8_t kEdges[4] = {5, 6, 8, 7};  // TL,TR,BR,BL
          for (int corner = 0; corner < 4; corner++) {
            const float radius = EffectiveCornerRadius(after, corner);
            const uint8_t is_percent =
                CornerRadiusPercent(after, corner) ? 1 : 0;
            update_radius(decorated, kEdges[corner], radius, radius,
                          is_percent, is_percent);
          }
        }
        if (!SameGradient(before, after)) {
          if (after.has_gradient) {
            if (update_gradient == nullptr) {
              rebuild = true;
              break;
            }
            update_gradient(decorated, BuildLinearGradientHandle(after));
          } else {
            if (clear_gradient == nullptr) {
              rebuild = true;
              break;
            }
            clear_gradient(decorated);
          }
        }
      }
      if (node->clip_widget_p != nullptr && RadiiChanged(before, after)) {
        if (update_clip_radius == nullptr) {
          rebuild = true;
          break;
        }
        const double tl = EffectiveCornerRadius(after, 0);
        const double tr = EffectiveCornerRadius(after, 1);
        const double br = EffectiveCornerRadius(after, 2);
        const double bl = EffectiveCornerRadius(after, 3);
        const uint8_t tl_pct = CornerRadiusPercent(after, 0) ? 1 : 0;
        const uint8_t tr_pct = CornerRadiusPercent(after, 1) ? 1 : 0;
        const uint8_t br_pct = CornerRadiusPercent(after, 2) ? 1 : 0;
        const uint8_t bl_pct = CornerRadiusPercent(after, 3) ? 1 : 0;
        update_clip_radius(Dart_HandleFromPersistent(node->clip_widget_p), tl,
                           tl, tr, tr, bl, bl, br, br, tl_pct, tl_pct, tr_pct,
                           tr_pct, bl_pct, bl_pct, br_pct, br_pct);
      }
      if (node->opacity_widget_p != nullptr && update_opacity != nullptr &&
          std::fabs(before.opacity - after.opacity) > 0.001) {
        Dart_Handle opacity =
            Dart_HandleFromPersistent(node->opacity_widget_p);
        update_opacity(opacity, after.opacity);
      }
      // CSS transform updates reuse the existing YGNode.  This also lets a
      // `transform` transition animate without rebuilding the page.
      const bool transform_changed =
          before.has_transform != after.has_transform ||
          std::memcmp(before.transform, after.transform,
                      sizeof(before.transform)) != 0 ||
          !SameFloatOrNan(before.transform_origin_x, after.transform_origin_x) ||
          !SameFloatOrNan(before.transform_origin_y, after.transform_origin_y) ||
          !SameFloatOrNan(before.transform_origin_x_percent,
                          after.transform_origin_x_percent) ||
          !SameFloatOrNan(before.transform_origin_y_percent,
                          after.transform_origin_y_percent);
      if (transform_changed && node->yoga_node_p != nullptr &&
          g_page != nullptr && g_page->holder != nullptr) {
        auto* setup = reinterpret_cast<YGNodeSetupTransformFn>(FindCallback(
            g_page->holder->ffi_table, kYGNodeSetupTransformHash));
        auto* origin = reinterpret_cast<YGNodeTransformOriginFn>(FindCallback(
            g_page->holder->ffi_table, kYGNodeTransformOriginHash));
        if (setup != nullptr) {
          Dart_Handle matrix = Dart_NewTypedData(Dart_TypedData_kFloat64, 16);
          if (!Dart_IsError(matrix)) {
            Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
            void* data = nullptr;
            intptr_t length = 0;
            if (!Dart_IsError(Dart_TypedDataAcquireData(
                    matrix, &data_type, &data, &length)) &&
                data_type == Dart_TypedData_kFloat64 && length == 16 &&
                data != nullptr) {
              auto* values = static_cast<double*>(data);
              for (int i = 0; i < 16; i++) {
                values[i] = static_cast<double>(after.transform[i]);
              }
            }
            Dart_TypedDataReleaseData(matrix);
            Dart_Handle yoga =
                Dart_HandleFromPersistent(node->yoga_node_p);
            setup(yoga, matrix);
            if (origin != nullptr) {
              double origin_x = 0.0;
              double origin_y = 0.0;
              ResolveTransformOrigin(after, node->measured_w, node->measured_h,
                                     &origin_x, &origin_y);
              origin(yoga, origin_x, origin_y, -1.0, -1.0);
            }
            DeletePersistent(&node->transform_p);
            node->transform_p = Dart_NewPersistentHandle(matrix);
          }
        }
      }
    }
    if (node->yoga_node_p != nullptr) {
      yoga_nodes.push_back(Dart_HandleFromPersistent(node->yoga_node_p));
    }
    node->previous_style = after;
    if (node->tag == "text") {
      std::string storage;
      node->previous_text = NodeText(node, &storage);
    }
  }
  if (rebuild) return false;

  const float viewport_w =
      g_viewport_width > 0.0 ? static_cast<float>(g_viewport_width) : 320.0f;
  const float viewport_h =
      g_viewport_height > 0.0 ? static_cast<float>(g_viewport_height) : 240.0f;
  MeasureNode(root, viewport_w, true, viewport_h, true);
  root->measured_w = viewport_w;
  root->measured_h = viewport_h;
  std::vector<NodeMetric> metrics;
  LayoutNode(root, 0.0f, 0.0f, viewport_w, viewport_h, &metrics);
  WriteLayoutMetrics(metrics);
  for (Dart_Handle widget : yoga_nodes) mark_needs_layout(widget);
  Dart_Handle root_handle = Dart_HandleFromPersistent(g_page->root_node);
  mark_needs_layout(root_handle);
  std::fprintf(stderr,
               "[skyline-runtime] appservice targeted update: %zu nodes\n",
               yoga_nodes.size());
  return true;
}

void RebuildAppServicePage(RuntimeHolder* holder) {
  ClearTransitions();
  if (g_page == nullptr || g_page->root_node == nullptr ||
      g_page->root_connector == nullptr) {
    return;
  }
  AppNode* root = FindNode(g_page->root_id);
  if (root == nullptr) return;

  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* newText =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newScrollView = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  auto* newListView = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  auto* newGridView = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  auto* newDirectRenderWidget = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  auto* newListBuilder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  auto* newGridBuilder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  auto* updateChildren = reinterpret_cast<ConnectorUpdateChildrenFn>(
      FindCallback(holder->ffi_table, kYGConnectorUpdateChildrenHash));
  auto* markNeedsLayout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  if (newYGNode == nullptr || newYGConnector == nullptr || newText == nullptr ||
      newTextSpan == nullptr || newDecoratedBox == nullptr ||
      newOpacity == nullptr || updateChildren == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] appservice flush: callbacks missing\n");
    return;
  }

  ResolveStyles(root, nullptr);
  const float viewport_w =
      g_viewport_width > 0.0 ? static_cast<float>(g_viewport_width) : 320.0f;
  const float viewport_h =
      g_viewport_height > 0.0 ? static_cast<float>(g_viewport_height) : 240.0f;
  for (auto& entry : g_page->nodes) {
    ResetNodeHandles(entry.second, true);
  }
  PrepareLayoutBuffers(root);
  MeasureNode(root, viewport_w, true, viewport_h, true);
  root->measured_w = viewport_w;
  root->measured_h = viewport_h;
  std::vector<NodeMetric> metrics;
  LayoutNode(root, 0.0f, 0.0f, viewport_w, viewport_h, &metrics);
  WriteLayoutMetrics(metrics);

  std::vector<Dart_Handle> widgets;
  ResetDeferredFixedNodes();
  g_fixed_root_node = root;
  g_collect_fixed_nodes = true;
  for (AppNode* child : root->children) {
    if (child->released || child->style.display_none) continue;
    if (DeferFixedChild(child)) continue;
    Dart_Handle widget = BuildWidget(child, newYGNode, newYGConnector, newText,
                                     newTextSpan, newDecoratedBox, newOpacity,
                                     newScrollView, newListView, newGridView,
                                     newDirectRenderWidget, newListBuilder,
                                     newGridBuilder, false);
    if (widget != nullptr && !Dart_IsError(widget)) widgets.push_back(widget);
  }
  for (size_t i = 0; i < g_deferred_fixed_nodes.size(); i++) {
    AppNode* fixed = g_deferred_fixed_nodes[i];
    if (fixed == nullptr || fixed->released || fixed->style.display_none) {
      continue;
    }
    std::vector<NodeMetric> fixed_metrics;
    fixed_metrics.push_back(NodeMetric{fixed, fixed->abs_x, fixed->abs_y,
                                       fixed->measured_w, fixed->measured_h});
    WriteLayoutMetrics(fixed_metrics);
    Dart_Handle widget =
        BuildWidget(fixed, newYGNode, newYGConnector, newText, newTextSpan,
                    newDecoratedBox, newOpacity, newScrollView, newListView,
                    newGridView, newDirectRenderWidget, newListBuilder,
                    newGridBuilder, false);
    if (widget != nullptr && !Dart_IsError(widget)) widgets.push_back(widget);
  }
  g_collect_fixed_nodes = false;
  ResetDeferredFixedNodes();
  Dart_Handle list = Dart_NewList(static_cast<intptr_t>(widgets.size()));
  for (size_t i = 0; i < widgets.size(); i++) {
    Dart_ListSetAt(list, static_cast<intptr_t>(i), widgets[i]);
  }
  Dart_Handle connector = Dart_HandleFromPersistent(g_page->root_connector);
  updateChildren(connector, 0, list);
  if (markNeedsLayout != nullptr) {
    for (Dart_Handle widget : widgets) markNeedsLayout(widget);
    Dart_Handle root_handle = Dart_HandleFromPersistent(g_page->root_node);
    markNeedsLayout(root_handle);
  }
  SnapshotNode(root);
  std::fprintf(stderr, "[skyline-runtime] appservice flush: %zu children\n",
               widgets.size());
}

void FlushAppServicePage(RuntimeHolder* holder, bool structural) {
  if (!structural && ApplyTargetedUpdates(holder)) return;
  RebuildAppServicePage(holder);
}

// -- CSS transitions and animated styles ------------------------------------

struct ActiveTransition {
  AppNode* node = nullptr;
  AppStyle from;
  AppStyle to;
  uint32_t mask = 0;
  double start_ms = 0.0;
  double duration_ms = 1.0;
  int easing = 0;
};

std::vector<ActiveTransition> g_active_transitions;

double SteadyNowMs() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point origin = clock::now();
  return std::chrono::duration<double, std::milli>(clock::now() - origin)
      .count();
}

double EaseTransition(int mode, double t) {
  if (t <= 0.0) return 0.0;
  if (t >= 1.0) return 1.0;
  switch (mode) {
    case 1:  // ease
      return t * t * (3.0 - 2.0 * t);
    case 2:  // ease-in
      return t * t;
    case 3:  // ease-out
      return 1.0 - (1.0 - t) * (1.0 - t);
    case 4:  // ease-in-out
      return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
    default:
      return t;
  }
}

double LerpDouble(double a, double b, double t) { return a + (b - a) * t; }

uint32_t LerpColor(uint32_t a, uint32_t b, double t) {
  auto channel = [t](uint32_t x, uint32_t y, int shift) -> uint32_t {
    const double v = LerpDouble((x >> shift) & 0xFF, (y >> shift) & 0xFF, t);
    return static_cast<uint32_t>(std::lround(v)) & 0xFF;
  };
  return (channel(a, b, 24) << 24) | (channel(a, b, 16) << 16) |
         (channel(a, b, 8) << 8) | channel(a, b, 0);
}

uint32_t TransitionChangedMask(const AppStyle& before, const AppStyle& after,
                               uint32_t mask) {
  uint32_t changed = 0;
  if ((mask & kTransitionOpacity) != 0 &&
      std::fabs(before.opacity - after.opacity) > 0.001) {
    changed |= kTransitionOpacity;
  }
  if ((mask & kTransitionBackground) != 0 &&
      (before.has_background != after.has_background ||
       before.background != after.background)) {
    changed |= kTransitionBackground;
  }
  if ((mask & kTransitionColor) != 0 && before.color != after.color) {
    changed |= kTransitionColor;
  }
  if ((mask & kTransitionBorderColor) != 0 &&
      BorderSidesChanged(before, after)) {
    changed |= kTransitionBorderColor;
  }
  if ((mask & kTransitionTransform) != 0 &&
      (before.has_transform != after.has_transform ||
       std::memcmp(before.transform, after.transform,
                   sizeof(before.transform)) != 0)) {
    changed |= kTransitionTransform;
  }
  return changed;
}

void ResetTransitionProperties(AppStyle* style, const AppStyle& from,
                              uint32_t mask) {
  if ((mask & kTransitionOpacity) != 0) style->opacity = from.opacity;
  if ((mask & kTransitionBackground) != 0) {
    style->has_background = from.has_background;
    style->background = from.background;
  }
  if ((mask & kTransitionColor) != 0) style->color = from.color;
  if ((mask & kTransitionBorderColor) != 0) {
    style->border_color = from.border_color;
    style->border_width = from.border_width;
    for (int side = 0; side < 4; side++) {
      style->side_border_width[side] = EffectiveBorderWidth(from, side);
      style->side_border_color[side] = EffectiveBorderColor(from, side);
      style->side_border_color_set[side] = true;
    }
  }
  if ((mask & kTransitionTransform) != 0) {
    style->has_transform = from.has_transform;
    std::memcpy(style->transform, from.transform, sizeof(style->transform));
  }
}

AppStyle InterpolateTransition(const ActiveTransition& transition, double t) {
  AppStyle style = transition.to;
  const AppStyle& from = transition.from;
  const AppStyle& to = transition.to;
  const uint32_t mask = transition.mask;
  if ((mask & kTransitionOpacity) != 0) {
    style.opacity = LerpDouble(from.opacity, to.opacity, t);
  }
  if ((mask & kTransitionBackground) != 0) {
    style.has_background = from.has_background || to.has_background;
    style.background = LerpColor(from.background, to.background, t);
  }
  if ((mask & kTransitionColor) != 0) {
    style.color = LerpColor(from.color, to.color, t);
  }
  if ((mask & kTransitionBorderColor) != 0) {
    style.border_color = LerpColor(from.border_color, to.border_color, t);
    style.border_width =
        static_cast<float>(LerpDouble(from.border_width, to.border_width, t));
    for (int side = 0; side < 4; side++) {
      style.side_border_width[side] = static_cast<float>(LerpDouble(
          EffectiveBorderWidth(from, side), EffectiveBorderWidth(to, side), t));
      style.side_border_color[side] =
          LerpColor(EffectiveBorderColor(from, side),
                    EffectiveBorderColor(to, side), t);
      style.side_border_color_set[side] = true;
    }
  }
  if ((mask & kTransitionTransform) != 0) {
    style.has_transform = from.has_transform || to.has_transform;
    for (int i = 0; i < 16; i++) {
      style.transform[i] = static_cast<float>(
          LerpDouble(from.transform[i], to.transform[i], t));
    }
  }
  return style;
}

void ScheduleTransitionTick(RuntimeHolder* holder);
void AdvanceTransitions(RuntimeHolder* holder);
void SchedulePendingAnimatedStyles(RuntimeHolder* holder);

void ClearTransitions() {
  for (ActiveTransition& transition : g_active_transitions) {
    if (transition.node != nullptr) {
      transition.node->animation_override_active = false;
    }
  }
  g_active_transitions.clear();
}

void StartTransitions(uint32_t root_id) {
  if (g_page == nullptr || root_id == 0) return;
  AppNode* root = FindNode(root_id);
  if (root == nullptr) return;
  ResolveStyles(root, nullptr);

  std::vector<AppNode*> pending;
  pending.push_back(root);
  while (!pending.empty()) {
    AppNode* node = pending.back();
    pending.pop_back();
    for (AppNode* child : node->children) {
      if (!child->released) pending.push_back(child);
    }
    if (!node->has_previous_style || node->animation_override_active) continue;
    const AppStyle& before = node->previous_style;
    const AppStyle& after = node->style;
    if (after.transition_duration <= 0.0 && after.transition_delay <= 0.0) {
      continue;
    }
    const uint32_t mask =
        TransitionChangedMask(before, after, after.transition_mask);
    if (mask == 0) continue;

    ActiveTransition transition;
    transition.node = node;
    transition.from = before;
    transition.to = after;
    transition.mask = mask;
    transition.start_ms = SteadyNowMs() + after.transition_delay;
    transition.duration_ms = std::max(1.0, after.transition_duration);
    transition.easing = after.transition_easing;
    node->animation_override = after;
    ResetTransitionProperties(&node->animation_override, before, mask);
    node->animation_override_active = true;
    g_active_transitions.push_back(transition);
    std::fprintf(stderr,
                 "[skyline-runtime] transition #%u start: mask=0x%x "
                 "duration=%gms delay=%gms\n",
                 node->id, mask, after.transition_duration,
                 after.transition_delay);
  }
  if (!g_active_transitions.empty() && g_page->holder != nullptr) {
    ScheduleTransitionTick(g_page->holder);
  }
}

void ScheduleTransitionTick(RuntimeHolder* holder) {
  if (g_active_transitions.empty()) return;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostDelayedTask(
      [holder]() {
        if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) return;
        DemoPageApiScope scope;
        AdvanceTransitions(holder);
      },
      fml::TimeDelta::FromMilliseconds(16));
}

void AdvanceTransitions(RuntimeHolder* holder) {
  if (g_active_transitions.empty() || g_page == nullptr) return;
  const double now = SteadyNowMs();
  bool pending_animation = false;
  bool applied = false;
  std::vector<AppNode*> finished;
  for (ActiveTransition& transition : g_active_transitions) {
    if (transition.node == nullptr || transition.node->released) {
      finished.push_back(transition.node);
      continue;
    }
    if (now < transition.start_ms) {
      pending_animation = true;
      continue;
    }
    double raw = (now - transition.start_ms) / transition.duration_ms;
    if (raw < 0.0) raw = 0.0;
    if (raw > 1.0) raw = 1.0;
    const double eased = EaseTransition(transition.easing, raw);
    transition.node->animation_override =
        InterpolateTransition(transition, eased);
    transition.node->animation_override_active = true;
    applied = true;
    if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] transition #%u t=%.3f opacity=%.3f "
                   "background=%08x\n",
                   transition.node->id, raw,
                   transition.node->animation_override.opacity,
                   transition.node->animation_override.has_background
                       ? transition.node->animation_override.background
                       : 0u);
    }
    if (raw >= 1.0) {
      finished.push_back(transition.node);
    } else {
      pending_animation = true;
    }
  }
  if (applied) FlushAppServicePage(holder, false);
  if (!finished.empty() && !g_active_transitions.empty()) {
    for (AppNode* node : finished) {
      if (node != nullptr) node->animation_override_active = false;
    }
    g_active_transitions.erase(
        std::remove_if(g_active_transitions.begin(), g_active_transitions.end(),
                       [&finished](const ActiveTransition& transition) {
                         return std::find(finished.begin(), finished.end(),
                                          transition.node) != finished.end();
                       }),
        g_active_transitions.end());
    for (AppNode* node : finished) {
      if (node != nullptr && !node->released) {
        std::fprintf(stderr, "[skyline-runtime] transition #%u done\n", node->id);
      }
    }
    FlushAppServicePage(holder, false);
  }
  if (pending_animation) ScheduleTransitionTick(holder);
}

void SchedulePendingAnimatedStyles(RuntimeHolder* holder) {
  if (g_page == nullptr || holder == nullptr) return;
  auto runner = UiTaskRunner();
  if (!runner) return;
  for (size_t i = g_page->scheduled_animated_styles;
       i < g_page->animated_styles.size(); i++) {
    const PendingAnimatedStyle entry = g_page->animated_styles[i];
    runner->PostDelayedTask(
        [holder, entry]() {
          if (g_page == nullptr) return;
          if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) return;
          DemoPageApiScope scope;
          AppNode* node = FindNode(entry.node_id);
          if (node == nullptr || node->released) return;
          AppStyle style = node->style;
          ApplyDeclarations(&style, ParseDeclarations(entry.declarations));
          node->animation_override = style;
          node->animation_override_active = true;
          FlushAppServicePage(holder, false);
          std::fprintf(stderr,
                       "[skyline-runtime] animated style #%u at %dms: %s\n",
                       entry.node_id, entry.at_ms, entry.declarations.c_str());
        },
        fml::TimeDelta::FromMilliseconds(std::max(0, entry.at_ms)));
    std::fprintf(stderr,
                 "[skyline-runtime] animated style scheduled: #%u in %dms\n",
                 entry.node_id, entry.at_ms);
  }
  g_page->scheduled_animated_styles = g_page->animated_styles.size();
}


// -- page routes -------------------------------------------------------------

struct PageBuilders {
  NewYGNodeFn yg_node = nullptr;
  NewYGConnectorFn yg_connector = nullptr;
  NewTextFn text = nullptr;
  NewTextSpanFn text_span = nullptr;
  NewDecoratedBoxFn decorated = nullptr;
  NewOpacityFn opacity = nullptr;
  NewScrollViewFn scroll = nullptr;
  NewListViewFn list = nullptr;
  NewGridViewFn grid = nullptr;
  NewDirectRenderWidgetFn direct = nullptr;
  NewListBuilderFn list_builder = nullptr;
  NewGridBuilderFn grid_builder = nullptr;
  CreatePageRouteFn create_route = nullptr;
};

PageBuilders LoadPageBuilders(RuntimeHolder* holder) {
  PageBuilders builders;
  builders.yg_node = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  builders.yg_connector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  builders.text =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  builders.text_span = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  builders.decorated = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  builders.opacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  builders.scroll = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  builders.list = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  builders.grid = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  builders.direct = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  builders.list_builder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  builders.grid_builder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  builders.create_route = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  return builders;
}

// Builds and pushes one page route for an `attach` operation.  Called while
// replaying the captured operation stream, so the node table already contains
// the page's final tree.
bool MaterializeAttachedPage(RuntimeHolder* holder,
                             const PageBuilders& builders, uint32_t root_id,
                             uint8_t open_type, int32_t page_id,
                             int32_t callback_id) {
  if (g_page == nullptr || builders.create_route == nullptr) return false;
  AppNode* root = FindNode(root_id);
  if (root == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: no attach root (root=%u)\n",
                 root_id);
    return false;
  }
  if (builders.yg_node == nullptr || builders.yg_connector == nullptr ||
      builders.text == nullptr || builders.text_span == nullptr ||
      builders.decorated == nullptr || builders.opacity == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: callbacks not registered\n");
    return false;
  }

  ResolveStyles(root, nullptr);
  const float viewport_w =
      g_viewport_width > 0.0 ? static_cast<float>(g_viewport_width) : 320.0f;
  const float viewport_h =
      g_viewport_height > 0.0 ? static_cast<float>(g_viewport_height) : 240.0f;
  PrepareLayoutBuffers(root);
  MeasureNode(root, viewport_w, true, viewport_h, true);
  root->measured_w = viewport_w;
  root->measured_h = viewport_h;
  std::vector<NodeMetric> metrics;
  LayoutNode(root, 0.0f, 0.0f, viewport_w, viewport_h, &metrics);
  WriteLayoutMetrics(metrics);

  ResetDeferredFixedNodes();
  g_fixed_root_node = root;
  g_collect_fixed_nodes = true;
  Dart_Handle root_widget =
      BuildWidget(root, builders.yg_node, builders.yg_connector, builders.text,
                  builders.text_span, builders.decorated, builders.opacity,
                  builders.scroll, builders.list, builders.grid, builders.direct,
                  builders.list_builder, builders.grid_builder, true);
  g_collect_fixed_nodes = false;
  ResetDeferredFixedNodes();
  if (root_widget == nullptr || Dart_IsError(root_widget)) {
    std::fprintf(stderr, "[skyline-runtime] appservice ops: build failed\n");
    return false;
  }

  static char page_url[] = "pages/appservice";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = page_url;
  Dart_Handle empty = Dart_NewStringFromCString("");
  g_page->root_id = root_id;
  g_page->page_count++;
  std::fprintf(stderr,
               "[skyline-runtime] appservice page %d -> CreatePageRoute "
               "(openType=%u, %gx%g)\n",
               g_page->page_count, static_cast<unsigned>(open_type),
               static_cast<double>(viewport_w), static_cast<double>(viewport_h));
  builders.create_route(callback_id, open_type, 0,
                        open_type == 1 /* kSwitchTab */ ? 1 : 0, page_id,
                        1, 1, 0, 0, 0, 0, 0, empty, 1, 1, 0, 0, 0, 0, 0, 0,
                        empty, empty, Dart_Null(), root_widget, &config);

  DeletePersistent(&g_page->root_node);
  DeletePersistent(&g_page->root_connector);
  g_page->root_node = Dart_NewPersistentHandle(root_widget);
  if (root->connector != nullptr) {
    g_page->root_connector = Dart_NewPersistentHandle(root->connector);
  }
  SnapshotNode(root);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    DebugDumpNode(root, 0);
    for (const NodeMetric& metric : metrics) {
      std::fprintf(stderr,
                   "[ops-debug] metrics #%u -> top=%g left=%g w=%g h=%g\n",
                   metric.node->id, static_cast<double>(metric.y),
                   static_cast<double>(metric.x), static_cast<double>(metric.w),
                   static_cast<double>(metric.h));
    }
  }
  return true;
}

void ApplyOpsPage(RuntimeHolder* holder, const char* descriptor) {
  const PageBuilders builders = LoadPageBuilders(holder);
  if (builders.yg_node == nullptr || builders.yg_connector == nullptr ||
      builders.create_route == nullptr || builders.text == nullptr ||
      builders.text_span == nullptr || builders.decorated == nullptr ||
      builders.opacity == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: callbacks not registered\n");
    return;
  }
  auto* navigateBack = reinterpret_cast<NavigateBackFn>(
      FindCallback(holder->ffi_table, kNavigateBackHash));
  auto* switchTab = reinterpret_cast<SwitchTabFn>(
      FindCallback(holder->ffi_table, kSwitchTabHash));

  std::string json;
  if (!ReadDescriptor(descriptor, &json)) {
    std::fprintf(stderr, "[skyline-runtime] appservice ops: cannot read %s\n",
                 descriptor);
    return;
  }
  g_ops_document = std::make_unique<rapidjson::Document>();
  rapidjson::Document& doc = *g_ops_document;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: parse error at %zu\n",
                 static_cast<size_t>(doc.GetErrorOffset()));
    return;
  }
  const auto ops_member = doc.FindMember("ops");
  if (ops_member == doc.MemberEnd() || !ops_member->value.IsArray()) {
    std::fprintf(stderr, "[skyline-runtime] appservice ops: missing ops array\n");
    return;
  }

  g_page = new OpPage();
  ClearTransitions();
  g_page->holder = holder;
  g_ui_task_runner = UiTaskRunner();
  g_next_rule_order = 0;
  g_next_address = 0x5000;
  g_builders_by_address.clear();
  g_image_nodes.clear();
  for (const rapidjson::Value& op : ops_member->value.GetArray()) {
    ApplyOperation(op);
    const char* kind = MemberText(op, "op");
    if (std::strcmp(kind, "attach") == 0) {
      const uint8_t open_type = g_page->pending_open_type != 0
                                    ? g_page->pending_open_type
                                    : (g_page->page_count == 0 ? 0 : 4);
      const int32_t page_id =
          g_page->pending_page_id != 0 ? g_page->pending_page_id
                                       : (g_page->page_count == 0 ? 0 : 1);
      MaterializeAttachedPage(holder, builders, g_page->root_id, open_type,
                              page_id, g_page->pending_callback_id);
      g_page->pending_open_type = 0;
      g_page->pending_page_id = 0;
      g_page->pending_callback_id = 0;
    } else if (std::strcmp(kind, "notifyNavigateBack") == 0) {
      if (g_page->page_count > 0 && navigateBack != nullptr) {
        navigateBack(g_page->pending_back_delta,
                     g_page->pending_back_animated ? 1 : 0, 0);
        std::fprintf(stderr,
                     "[skyline-runtime] NavigateBack: delta=%d animated=%d\n",
                     g_page->pending_back_delta,
                     g_page->pending_back_animated ? 1 : 0);
      }
      g_page->pending_open_type = 0;
    } else if (std::strcmp(kind, "notifySwitchTab") == 0) {
      if (g_page->page_count > 0 && switchTab != nullptr) {
        switchTab(g_page->pending_page_id, 0);
        std::fprintf(stderr, "[skyline-runtime] SwitchTab: page=%d\n",
                     g_page->pending_page_id);
        g_page->pending_open_type = 0;
      }
    }
  }
  if (g_page->page_count == 0 && g_page->root_id != 0) {
    MaterializeAttachedPage(holder, builders, g_page->root_id, 0, 0, 0);
  }
  if (g_page->page_count == 0) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: no attach root (root=%u)\n",
                 g_page->root_id);
    return;
  }
  SchedulePendingAnimatedStyles(holder);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    DebugDumpNode(FindNode(g_page->root_id), 0);
  }
  std::fprintf(stderr,
               "[skyline-runtime] appservice ops: %d ops, %u nodes -> "
               "CreatePageRoute (%d page(s))\n",
               g_page->applied_ops,
               static_cast<unsigned>(g_page->nodes.size()), g_page->page_count);

  // @font-face requests go through the real Dart font pipeline:
  // LoadFontFace -> ResourceLoader.loadResourceAsync -> LoadResourceAsync ->
  // NotifyResourceLoad -> Flutter FontLoader -> NotifyFontFaceLoad.
  if (!g_page->fonts.empty()) {
    auto* load_font_face = reinterpret_cast<LoadFontFaceFn>(
        FindCallback(holder->ffi_table, kLoadFontFaceHash));
    static int32_t font_callback_id = 0;
    for (const FontFaceRequest& font : g_page->fonts) {
      if (load_font_face == nullptr) break;
      Dart_Handle family = Dart_NewStringFromCString(font.family.c_str());
      Dart_Handle urls = Dart_NewList(1);
      Dart_Handle url = Dart_NewStringFromCString(font.url.c_str());
      if (Dart_IsError(family) || Dart_IsError(urls) || Dart_IsError(url)) {
        continue;
      }
      Dart_ListSetAt(urls, 0, url);
      std::fprintf(stderr, "[skyline-runtime] appservice font face: %s -> %s\n",
                   font.family.c_str(), font.url.c_str());
      load_font_face(family, urls, 1000, ++font_callback_id);
    }
  }

  // `updates` batches replay the appservice flush loop: apply a group of
  // operations on the UI task runner, then update the attached tree.
  const auto updates_member = doc.FindMember("updates");
  if (updates_member != doc.MemberEnd() && updates_member->value.IsArray()) {
    auto task_runner = UiTaskRunner();
    for (const rapidjson::Value& batch : updates_member->value.GetArray()) {
      if (!batch.IsObject()) continue;
      const auto ops_member = batch.FindMember("ops");
      if (ops_member == batch.MemberEnd() || !ops_member->value.IsArray()) {
        continue;
      }
      const int delay_ms =
          static_cast<int>(MemberNumber(batch, "afterMs", 300.0f));
      const rapidjson::Value* ops = &ops_member->value;
      if (!task_runner) continue;
      task_runner->PostDelayedTask(
          [holder, ops, builders, navigateBack, switchTab]() {
            if (g_page == nullptr ||
                (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr)) {
              return;
            }
            DemoPageApiScope scope;
            bool structural = false;
            bool needs_flush = false;
            bool created_route = false;
            bool builder_structural_only = true;
            bool general_dirty = false;
            for (const rapidjson::Value& op : ops->GetArray()) {
              ApplyOperation(op);
              const auto kind = op.FindMember("op");
              if (kind == op.MemberEnd() || !kind->value.IsString()) continue;
              const char* name = kind->value.GetString();
              if (std::strcmp(name, "attach") == 0) {
                const uint8_t open_type = g_page->pending_open_type != 0
                                              ? g_page->pending_open_type
                                              : 4;
                const int32_t page_id =
                    g_page->pending_page_id != 0 ? g_page->pending_page_id : 1;
                if (MaterializeAttachedPage(holder, builders, g_page->root_id,
                                            open_type, page_id,
                                            g_page->pending_callback_id)) {
                  created_route = true;
                }
                g_page->pending_open_type = 0;
                g_page->pending_page_id = 0;
                g_page->pending_callback_id = 0;
              } else if (std::strcmp(name, "notifyNavigateBack") == 0) {
                if (navigateBack != nullptr && g_page->page_count > 0) {
                  navigateBack(g_page->pending_back_delta,
                               g_page->pending_back_animated ? 1 : 0, 0);
                  std::fprintf(
                      stderr,
                      "[skyline-runtime] NavigateBack: delta=%d animated=%d\n",
                      g_page->pending_back_delta,
                      g_page->pending_back_animated ? 1 : 0);
                }
                g_page->pending_open_type = 0;
              } else if (std::strcmp(name, "notifySwitchTab") == 0) {
                if (switchTab != nullptr && g_page->page_count > 0) {
                  switchTab(g_page->pending_page_id, 0);
                  std::fprintf(stderr, "[skyline-runtime] SwitchTab: page=%d\n",
                               g_page->pending_page_id);
                  g_page->pending_open_type = 0;
                }
              } else if (std::strcmp(name, "dispatchTouch") != 0 &&
                         std::strcmp(name, "startRender") != 0) {
                if (IsStructuralOpKind(name)) {
                  structural = true;
                  if (std::strcmp(name, "createElement") != 0) {
                    AppNode* target = FindNode(MemberUint(op, "id", 0));
                    if (target == nullptr || !IsBuilderSubtree(target)) {
                      builder_structural_only = false;
                    }
                  }
                } else {
                  AppNode* target = FindNode(MemberUint(op, "id", 0));
                  if (target != nullptr && IsBuilderSubtree(target)) {
                    // Builder sync handles this; do not mark the batch as a
                    // general style/text change.
                  } else {
                    general_dirty = true;
                  }
                }
                needs_flush = true;
              }
            }
            const bool synced = SyncAppServiceBuilders(holder);
            if (g_page->force_rebuild) {
              structural = true;
              g_page->force_rebuild = false;
            }
            if (structural && builder_structural_only && synced) {
              structural = false;
            }
            if (needs_flush && !created_route) {
              if (!structural && !general_dirty && synced) {
                // The builder callbacks already applied the entire batch.
              } else {
                if (structural) {
                  ClearTransitions();
                } else {
                  StartTransitions(g_page->root_id);
                }
                SchedulePendingAnimatedStyles(holder);
                FlushAppServicePage(holder, structural);
              }
            }
          },
          fml::TimeDelta::FromMilliseconds(delay_ms));
      std::fprintf(stderr,
                   "[skyline-runtime] appservice update in %d ms (%u ops)\n",
                   delay_ms, static_cast<unsigned>(ops->GetArray().Size()));
    }
  }
}

// Synthetic keyboard input: append/delete text on the focused node and push
// the change through the normal fine-grained update path.
void HandleKeyInput(RuntimeHolder* holder, int32_t kind, uint32_t logical,
                    const std::string& text) {
  if (g_page == nullptr || kind == 1 /* key up */) return;
  AppNode* input = FindNode(g_page->focused_input_id);
  if (input == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] key event ignored: no focused input "
                 "(kind=%d logical=%u)\n",
                 static_cast<int>(kind), logical);
    return;
  }
  ClampInputState(input);
  int start = input->input_selection_start;
  int end = input->input_selection_end;
  if (start > end) std::swap(start, end);
  if (start == end) start = end = input->input_cursor;
  const bool has_selection = start != end;
  const int length = Utf8CodepointCount(input->text);
  auto erase_selection = [&]() {
    if (has_selection) {
      Utf8Erase(&input->text, start, end);
      input->input_cursor = start;
    }
  };

  bool changed = false;
  bool handled = true;
  switch (logical) {
    case 0x00000008u:  // backspace
      if (has_selection) {
        erase_selection();
        changed = true;
      } else if (input->input_cursor > 0) {
        Utf8Erase(&input->text, input->input_cursor - 1,
                  input->input_cursor);
        input->input_cursor--;
        changed = true;
      }
      break;
    case 46u:  // delete
      if (has_selection) {
        erase_selection();
        changed = true;
      } else if (input->input_cursor < length) {
        Utf8Erase(&input->text, input->input_cursor,
                  input->input_cursor + 1);
        changed = true;
      }
      break;
    case 37u:  // left
      input->input_cursor = has_selection ? start : std::max(0, start - 1);
      changed = true;
      break;
    case 38u:  // up: move to the start of the value
      input->input_cursor = 0;
      changed = true;
      break;
    case 39u:  // right
      input->input_cursor = has_selection ? end : std::min(length, end + 1);
      changed = true;
      break;
    case 40u:  // down: move to the end of the value
      input->input_cursor = length;
      changed = true;
      break;
    case 36u:  // home
      input->input_cursor = 0;
      changed = true;
      break;
    case 35u:  // end
      input->input_cursor = length;
      changed = true;
      break;
    case 13u:  // enter
      if (!input->input_hold_keyboard) {
        if (input->input_widget_p != nullptr && g_page->holder != nullptr) {
          auto* focus = reinterpret_cast<InputFocusFn>(FindCallback(
              g_page->holder->ffi_table, kInputFocusHash));
          if (focus != nullptr) {
            focus(Dart_HandleFromPersistent(input->input_widget_p), 0);
          }
        }
        std::fprintf(stderr,
                     "[skyline-runtime] key input: id=%u confirm, "
                     "InputHoldKeyboard hold=0\n",
                     input->id);
        g_page->focused_input_id = 0;
        return;
      }
      changed = true;
      break;
    case 27u:  // escape
      g_page->focused_input_id = 0;
      return;
    default:
      if (!text.empty()) {
        erase_selection();
        const int position = input->input_cursor;
        Utf8Insert(&input->text, position, text);
        input->input_cursor = position + Utf8CodepointCount(text);
        input->composing = false;
        input->composition_base_text.clear();
        changed = true;
      } else {
        handled = false;
      }
      break;
  }
  if (!handled) return;
  CollapseInputSelection(input);
  ClampInputState(input);
  if (!changed) return;
  NotifyInputState(input);
  std::fprintf(stderr,
               "[skyline-runtime] key input: id=%u value=\"%s\" cursor=%d "
               "selection=[%d,%d]\n",
               input->id, input->text.c_str(), input->input_cursor,
               input->input_selection_start, input->input_selection_end);
  FlushAppServicePage(holder, false);
}

}  // namespace appservice

SKYLINE_EXPORT extern "C" void DispatchSkylineKeyEvent(int32_t kind,
                                                       uint32_t logical,
                                                       const char* text) {
  std::fprintf(stderr,
               "[skyline-runtime] DispatchSkylineKeyEvent: kind=%d logical=%u "
               "text=%s\n",
               static_cast<int>(kind), logical, text != nullptr ? text : "");
  if (appservice::g_page == nullptr || appservice::g_page->holder == nullptr) {
    return;
  }
  const std::string value = text != nullptr ? text : "";
  if (Dart_CurrentIsolate() != nullptr) {
    // Called with the isolate already entered (e.g. from a Dart callback).
    DemoPageApiScope scope;
    appservice::HandleKeyInput(appservice::g_page->holder, kind, logical,
                               value);
    return;
  }
  RuntimeHolder* holder = appservice::g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) runner = appservice::g_ui_task_runner;
  if (!runner) return;
  // The headless embedder runs the UI tasks on the same thread that drives the
  // engine, so run inline instead of deadlocking behind the host's wait loop.
  fml::TaskRunner::RunNowOrPostTask(
      runner, [holder, kind, logical, value]() {
        DemoPageApiScope scope;
        appservice::HandleKeyInput(holder, kind, logical, value);
      });
}


void CreateAppServiceOpsPage(RuntimeHolder* holder, const char* descriptor) {
  appservice::ApplyOpsPage(holder, descriptor);
}


void ScheduleCreateDemoPage(RuntimeHolder* holder) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
#pragma GCC diagnostic pop
  if (!task_runner) {
    std::fprintf(stderr, "[skyline-runtime] demo page: no UI task runner\n");
    return;
  }
  task_runner->PostTask([holder]() { CreateDemoPage(holder); });
}

#endif  // SKYLINE_EMBEDDED

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" void LogVerbose(void* tag, void* message) {
  LogTwo("V", static_cast<SkylineHandle>(tag), static_cast<SkylineHandle>(message));
}
SKYLINE_EXPORT extern "C" void LogDebug(void* tag, void* message) {
  LogTwo("D", static_cast<SkylineHandle>(tag), static_cast<SkylineHandle>(message));
}
SKYLINE_EXPORT extern "C" void LogInfo(void* tag, void* message) {
  LogTwo("I", static_cast<SkylineHandle>(tag), static_cast<SkylineHandle>(message));
}
SKYLINE_EXPORT extern "C" void LogWarning(void* tag, void* message) {
  LogTwo("W", static_cast<SkylineHandle>(tag), static_cast<SkylineHandle>(message));
}
SKYLINE_EXPORT extern "C" void LogError(void* tag, void* message) {
  LogTwo("E", static_cast<SkylineHandle>(tag), static_cast<SkylineHandle>(message));
}

// ---------------------------------------------------------------------------
// Input / routing platform channels
//
// Dart reports mouse/touch events and page-route transitions to the native
// runtime; the official implementation forwards them to the appservice JS.
// The replay has no JS peer, so these log the payload under
// SKYLINE_APP_SERVICE_DEBUG and otherwise keep the channel wired (the Dart
// side calls them unconditionally).
// ---------------------------------------------------------------------------

namespace {

bool DebugChannels() {
  return std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr;
}

const char* DartStringOrNull(SkylineHandle value) {
#ifdef SKYLINE_EMBEDDED
  if (value != nullptr && Dart_IsString(value)) {
    const char* text = nullptr;
    if (!Dart_IsError(Dart_StringToCString(value, &text))) return text;
  }
#endif
  return nullptr;
}

// Layout mirrors `NativeMouseEvent` in package:skyline/gestures/mouse_event.dart.
struct NativeMouseEventLayout {
  intptr_t target;
  const char* type;
  double client_x;
  double client_y;
  double screen_x;
  double screen_y;
  double page_x;
  double page_y;
  int64_t time_stamp;
  int32_t bubbles;
  int32_t cancelable;
  int32_t composed;
};

// Layout mirrors `NativeTouch` in package:skyline/gestures/touch_event.dart.
struct NativeTouchLayout {
  intptr_t target;
  int64_t identifier;
  double x;
  double y;
  double client_x;
  double client_y;
};

// Layout mirrors `NativeTouchEvent` (a batch of touches plus event metadata).
struct NativeTouchEventLayout {
  const NativeTouchLayout* touches;
  int32_t touch_length;
  const NativeTouchLayout* changed_touches;
  int32_t changed_touches_length;
  const char* type;
  intptr_t target;
  uint8_t target_type;
  int64_t time_stamp;
  int32_t bubbles;
  int32_t cancelable;
  int32_t composed;
};

}  // namespace

SKYLINE_EXPORT extern "C" void HandleMouseEvent(void* /*runtime_ptr*/,
                                                void* event) {
  if (!DebugChannels() || event == nullptr) return;
  const auto* mouse = static_cast<const NativeMouseEventLayout*>(event);
  std::fprintf(stderr,
               "[skyline-runtime] HandleMouseEvent: target=0x%lx type=%s "
               "client=(%g,%g) screen=(%g,%g)\n",
               static_cast<long>(mouse->target),
               mouse->type != nullptr ? mouse->type : "?",
               mouse->client_x, mouse->client_y, mouse->screen_x,
               mouse->screen_y);
}

SKYLINE_EXPORT extern "C" void HandleTouchEvent(void* /*runtime_ptr*/,
                                                void* event) {
  if (!DebugChannels() || event == nullptr) return;
  const auto* batch = static_cast<const NativeTouchEventLayout*>(event);
  const NativeTouchLayout* first =
      batch->touches != nullptr && batch->touch_length > 0 ? batch->touches
                                                           : nullptr;
  std::fprintf(stderr,
               "[skyline-runtime] HandleTouchEvent: type=%s target=0x%lx "
               "touches=%d first=(id=%ld client=(%g,%g))\n",
               batch->type != nullptr ? batch->type : "?",
               static_cast<long>(batch->target), batch->touch_length,
               first != nullptr ? static_cast<long>(first->identifier) : -1L,
               first != nullptr ? first->client_x : 0.0,
               first != nullptr ? first->client_y : 0.0);
}

SKYLINE_EXPORT extern "C" void HandlePageRoute(void* /*runtime_ptr*/,
                                               void* name, double duration,
                                               uint8_t route_type) {
  if (!DebugChannels()) return;
  const char* text = DartStringOrNull(static_cast<SkylineHandle>(name));
  std::fprintf(stderr,
               "[skyline-runtime] HandlePageRoute: name=%s duration=%g type=%u\n",
               text != nullptr ? text : "?", duration,
               static_cast<unsigned>(route_type));
}

SKYLINE_EXPORT extern "C" void HandlePageRouteDone(void* /*runtime_ptr*/,
                                                   int32_t page_id) {
  if (!DebugChannels()) return;
  std::fprintf(stderr, "[skyline-runtime] HandlePageRouteDone: pageId=%d\n",
               page_id);
}

SKYLINE_EXPORT extern "C" void NotifyUserGestureInProgressChange(
    void* /*runtime_ptr*/, void* name, uint8_t in_progress) {
  if (!DebugChannels()) return;
  const char* text = DartStringOrNull(static_cast<SkylineHandle>(name));
  std::fprintf(stderr,
               "[skyline-runtime] NotifyUserGestureInProgressChange: %s=%d\n",
               text != nullptr ? text : "?", static_cast<int>(in_progress));
}

// ---------------------------------------------------------------------------
// Feature toggles
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" uint8_t GetFeatureToggle(const char* feature) {
  // Every Skyline feature toggle defaults to false; the Dart side then uses its
  // documented default behaviour (e.g. Dart-side borders, no anchor refactor).
  (void)feature;
  return 0;
}

#ifdef SKYLINE_EMBEDDED
// ---------------------------------------------------------------------------
// Resource loading (platform channel)
//
// `package:skyline` calls these when a font/image resource is requested.  Paths
// resolve against `SKYLINE_RESOURCE_ROOT` (absolute paths and file:// are used
// as-is).  Async results are delivered on the UI task runner by invoking the
// registered `NotifyResourceLoad` callback; calling back into Dart inside the
// original FFI call would violate the VM's callback rules.
// ---------------------------------------------------------------------------

namespace {

std::string ResourcePathFromDart(Dart_Handle value) {
  if (!Dart_IsString(value)) return {};
  const char* text = nullptr;
  if (Dart_IsError(Dart_StringToCString(value, &text)) || text == nullptr) {
    return {};
  }
  return text;
}

std::string ResolveResourcePath(const std::string& path) {
  std::string resolved = path;
  if (resolved.rfind("file://", 0) == 0) {
    resolved = resolved.substr(7);
  }
  if (resolved.empty() || resolved[0] == '/') return resolved;
  const char* root = std::getenv("SKYLINE_RESOURCE_ROOT");
  if (root == nullptr || *root == '\0') return resolved;
  std::string base = root;
  if (!base.empty() && base.back() != '/') base.push_back('/');
  return base + resolved;
}

bool ReadResourceBytes(const std::string& path, std::vector<uint8_t>* out) {
  if (path.empty()) return false;
  if (path.find("../") != std::string::npos ||
      path.find("/..") != std::string::npos) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  out->assign(std::istreambuf_iterator<char>(file),
              std::istreambuf_iterator<char>());
  return true;
}

Dart_Handle NewResourceData(const std::vector<uint8_t>& bytes) {
  Dart_Handle data =
      Dart_NewTypedData(Dart_TypedData_kUint8,
                        static_cast<intptr_t>(bytes.size()));
  if (Dart_IsError(data)) return data;
  if (bytes.empty()) return data;
  Dart_TypedData_Type type = Dart_TypedData_kInvalid;
  void* raw = nullptr;
  intptr_t length = 0;
  Dart_Handle status = Dart_TypedDataAcquireData(data, &type, &raw, &length);
  if (!Dart_IsError(status) && type == Dart_TypedData_kUint8 &&
      raw != nullptr && length == static_cast<intptr_t>(bytes.size())) {
    std::memcpy(raw, bytes.data(), bytes.size());
  }
  Dart_TypedDataReleaseData(data);
  return data;
}

// Font/resource cache.  `package:skyline` may request the same font or asset
// repeatedly; keep small payloads in memory for the replay lifetime.
std::unordered_map<std::string, std::vector<uint8_t>>& ResourceCache() {
  static auto* cache =
      new std::unordered_map<std::string, std::vector<uint8_t>>();
  return *cache;
}

bool LoadResourceBytes(const std::string& path, std::vector<uint8_t>* out,
                       bool* from_cache) {
  if (from_cache != nullptr) *from_cache = false;
  auto& cache = ResourceCache();
  const auto found = cache.find(path);
  if (found != cache.end()) {
    *out = found->second;
    if (from_cache != nullptr) *from_cache = true;
    return true;
  }
  if (!ReadResourceBytes(path, out)) return false;
  constexpr size_t kMaxCachedBytes = 4u * 1024u * 1024u;
  if (out->size() <= kMaxCachedBytes) cache.emplace(path, *out);
  return true;
}

}  // namespace

SKYLINE_EXPORT extern "C" Dart_Handle LoadResourceSync(void* /*runtime_ptr*/,
                                                       Dart_Handle path) {
  const std::string requested = ResourcePathFromDart(path);
  const std::string resolved = ResolveResourcePath(requested);
  std::vector<uint8_t> bytes;
  bool from_cache = false;
  const bool ok = LoadResourceBytes(resolved, &bytes, &from_cache);
  std::fprintf(stderr,
               "[skyline-runtime] LoadResourceSync: %s -> %s (%s, %zu bytes%s)\n",
               requested.c_str(), resolved.c_str(), ok ? "ok" : "missing",
               bytes.size(), from_cache ? ", cache" : "");
  return NewResourceData(bytes);
}

SKYLINE_EXPORT extern "C" void LoadResourceAsync(void* runtime_ptr,
                                                 void* /*window_ptr*/,
                                                 Dart_Handle path,
                                                 int32_t callback_id) {
  const std::string requested = ResourcePathFromDart(path);
  auto* runtime = static_cast<Runtime*>(runtime_ptr);
  RuntimeHolder* holder = runtime != nullptr ? runtime->holder : &g_fallback_holder;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, requested, callback_id]() {
    DemoPageApiScope scope;
    const std::string resolved = ResolveResourcePath(requested);
    std::vector<uint8_t> bytes;
    bool from_cache = false;
    const bool ok = LoadResourceBytes(resolved, &bytes, &from_cache);
    Dart_Handle payload = NewResourceData(bytes);
    void* callback = FindCallback(holder != nullptr ? holder->ffi_table : nullptr,
                                  kNotifyResourceLoadHash);
    if (callback != nullptr && payload != nullptr && !Dart_IsError(payload)) {
      reinterpret_cast<NotifyResourceLoadFn>(callback)(
          static_cast<uint8_t>(callback_id), payload);
    }
    std::fprintf(stderr,
                 "[skyline-runtime] LoadResourceAsync: %s -> %s (%s, %zu bytes, "
                 "id=%d, notified=%s%s)\n",
                 requested.c_str(), resolved.c_str(), ok ? "ok" : "missing",
                 bytes.size(), callback_id, callback != nullptr ? "yes" : "no",
                 from_cache ? ", cache" : "");
  });
}

SKYLINE_EXPORT extern "C" void NotifyFontFaceLoad(void* /*runtime_ptr*/,
                                                  int32_t callback_id,
                                                  int32_t status) {
  std::fprintf(stderr,
               "[skyline-runtime] NotifyFontFaceLoad: id=%d status=%d\n",
               callback_id, status);
}

// ---------------------------------------------------------------------------
// HTTP platform channel
//
// `HttpForwarder.httpGet` (package:skyline/utils/http_forwarder.dart) calls the
// `ForwardHttpGet` export.  The actual request is performed by Dart's own
// `RequestHttpResource` implementation (`dart:io` HttpClient for DevTools), so
// the native side only bridges the two: it hands Dart a C callback and later
// forwards the payload to `OnForwardHttpGetResponse` on the UI runner.
// ---------------------------------------------------------------------------

namespace {

using RequestHttpResourceFn = void (*)(uint32_t request_context_id,
                                       Dart_Handle url,
                                       void* native_callback);
using OnForwardHttpGetResponseFn = void (*)(Dart_Handle data,
                                            Dart_Handle callback);

struct PendingHttpGet {
  Dart_PersistentHandle callback = nullptr;
  RuntimeHolder* holder = nullptr;
};

std::unordered_map<uint32_t, PendingHttpGet> g_pending_http_gets;
uint32_t g_next_http_get_id = 0;

extern "C" void OnHttpResourceResponse(intptr_t /*runtime_ptr*/,
                                       uint32_t request_id, Dart_Handle data,
                                       uint8_t /*from_cache*/) {
  auto found = g_pending_http_gets.find(request_id);
  if (found == g_pending_http_gets.end()) return;
  PendingHttpGet pending = found->second;
  g_pending_http_gets.erase(found);
  Dart_PersistentHandle data_persistent = nullptr;
  if (data != nullptr && !Dart_IsNull(data) && !Dart_IsError(data)) {
    data_persistent = Dart_NewPersistentHandle(data);
  }
  auto runner = UiTaskRunner();
  if (!runner) {
    if (data_persistent != nullptr) Dart_DeletePersistentHandle(data_persistent);
    if (pending.callback != nullptr) Dart_DeletePersistentHandle(pending.callback);
    return;
  }
  runner->PostTask([pending, data_persistent]() {
    DemoPageApiScope scope;
    void* callback = FindCallback(
        pending.holder != nullptr ? pending.holder->ffi_table : nullptr,
        kOnForwardHttpGetResponseHash);
    if (callback != nullptr && pending.callback != nullptr) {
      Dart_Handle payload = data_persistent != nullptr
                                ? Dart_HandleFromPersistent(data_persistent)
                                : Dart_Null();
      Dart_Handle dart_callback =
          Dart_HandleFromPersistent(pending.callback);
      reinterpret_cast<OnForwardHttpGetResponseFn>(callback)(payload,
                                                             dart_callback);
    }
    if (data_persistent != nullptr) Dart_DeletePersistentHandle(data_persistent);
    if (pending.callback != nullptr) Dart_DeletePersistentHandle(pending.callback);
  });
}

}  // namespace

SKYLINE_EXPORT extern "C" void ForwardHttpGet(void* runtime_ptr,
                                              void* /*window_ptr*/,
                                              Dart_Handle url,
                                              Dart_Handle callback) {
  if (callback == nullptr || Dart_IsNull(callback)) return;
  auto* runtime = static_cast<Runtime*>(runtime_ptr);
  RuntimeHolder* holder = runtime != nullptr ? runtime->holder : &g_fallback_holder;
  const std::string requested = ResourcePathFromDart(url);
  const uint32_t request_id = ++g_next_http_get_id;
  PendingHttpGet pending;
  pending.callback = Dart_NewPersistentHandle(callback);
  pending.holder = holder;
  g_pending_http_gets[request_id] = pending;

  auto runner = UiTaskRunner();
  if (!runner) {
    g_pending_http_gets.erase(request_id);
    Dart_DeletePersistentHandle(pending.callback);
    return;
  }
  std::fprintf(stderr, "[skyline-runtime] ForwardHttpGet: id=%u url=%s\n",
               request_id, requested.c_str());
  runner->PostTask([holder, request_id, requested]() {
    DemoPageApiScope scope;
    void* request = FindCallback(holder != nullptr ? holder->ffi_table : nullptr,
                                 kRequestHttpResourceHash);
    if (request == nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] ForwardHttpGet: RequestHttpResource "
                   "callback missing\n");
      OnHttpResourceResponse(0, request_id, Dart_Null(), 0);
      return;
    }
    Dart_Handle url_handle = Dart_NewStringFromCString(requested.c_str());
    reinterpret_cast<RequestHttpResourceFn>(request)(
        request_id, url_handle,
        reinterpret_cast<void*>(&OnHttpResourceResponse));
  });
}
#endif  // SKYLINE_EMBEDDED

#ifdef SKYLINE_EMBEDDED
SKYLINE_EXPORT extern "C" void HandleGestureEvent(void* /*runtime_ptr*/,
                                                  int32_t handler_id,
                                                  int32_t gesture_type,
                                                  void* /*data*/,
                                                  int32_t /*bytes*/) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] HandleGestureEvent: handler=%d type=%d\n",
                 handler_id, gesture_type);
  }
}
#endif
// ---------------------------------------------------------------------------
// Generic void exports (arguments ignored)
// ---------------------------------------------------------------------------

#define SKYLINE_VOID_STUB(name) SKYLINE_EXPORT extern "C" void name(...) {}

// clang-format off
SKYLINE_VOID_STUB(AddPendingFont)
SKYLINE_VOID_STUB(BorderPaint)
SKYLINE_VOID_STUB(DidChangeAppLifecycleState)
SKYLINE_VOID_STUB(DidHaveMemoryPressure)
SKYLINE_VOID_STUB(DoIDKeyReport)
SKYLINE_VOID_STUB(DoKVReport)
SKYLINE_VOID_STUB(EvaluateJavaScript)
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(ForwardHttpGet)
#endif
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(HandleGestureEvent)
#endif
SKYLINE_VOID_STUB(HandleNativeViewPointerEvent)
SKYLINE_VOID_STUB(InitializeUITaskRunner)
SKYLINE_VOID_STUB(InstallJSEngineFromDart)
SKYLINE_VOID_STUB(LayoutParagraphMarkNeedsLayout)
SKYLINE_VOID_STUB(LayoutParagraphPaint)
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(LoadResourceAsync)
SKYLINE_VOID_STUB(NotifyFontFaceLoad)
#endif
SKYLINE_VOID_STUB(NotifyNavigateBackByGesture)
SKYLINE_VOID_STUB(NotifyNavigateBackDoneByGesture)
SKYLINE_VOID_STUB(NotifyWindowReady)
SKYLINE_VOID_STUB(OnAccessibilityFeaturesChanged)
SKYLINE_VOID_STUB(OnLayoutFinished)
SKYLINE_VOID_STUB(OnNavigateBackIntercept)
SKYLINE_VOID_STUB(OnViewportChanged)
SKYLINE_VOID_STUB(RemovePendingFont)
SKYLINE_VOID_STUB(ReportFrameTiming)
SKYLINE_VOID_STUB(ReportPerformance)
SKYLINE_VOID_STUB(SceneToImage)
SKYLINE_VOID_STUB(SetDevicePixelRatio)
SKYLINE_VOID_STUB(TraceBeginDart)
SKYLINE_VOID_STUB(TraceEndDart)
SKYLINE_VOID_STUB(TraceFlushDart)
SKYLINE_VOID_STUB(TraceInstantDart)
SKYLINE_VOID_STUB(UpdateAccessibility)
SKYLINE_VOID_STUB(UpdatePlatformBrightness)
SKYLINE_VOID_STUB(YogaTriggerIntersection)
SKYLINE_VOID_STUB(YogaTriggerIntersectionObservers)
// clang-format on

// ---------------------------------------------------------------------------
// Generic pointer exports (no layout/paint state yet)
// ---------------------------------------------------------------------------

#define SKYLINE_PTR_STUB(name) SKYLINE_EXPORT extern "C" intptr_t name(...) { return 0; }

// clang-format off
SKYLINE_PTR_STUB(GridViewMountChild)
SKYLINE_PTR_STUB(LayoutParagraphComputeSemanticsInfo)
SKYLINE_PTR_STUB(LayoutParagraphGetBoxesForRange)
SKYLINE_PTR_STUB(LayoutParagraphGetSpanForOffset)
SKYLINE_PTR_STUB(ListViewMountChild)
#ifndef SKYLINE_EMBEDDED
SKYLINE_PTR_STUB(LoadResourceSync)
#endif
SKYLINE_PTR_STUB(ScrollViewMountChild)
// clang-format on

SKYLINE_EXPORT extern "C" void* GetSplashCacheImage(...) {
  return nullptr;  // no splash cache in the stub runtime
}

// ---------------------------------------------------------------------------
// Scalar / struct returns
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" double AdjustDecelerationVelocity(
    void* /*runtime*/, int32_t /*view_tag*/, double velocity) {
  // Identity: keep the platform velocity; JavaScript-side customization is a
  // later milestone.
  return velocity;
}

SKYLINE_EXPORT extern "C" int32_t ShouldAcceptGesture(void* /*runtime*/,
                                                     int32_t handler_id) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] ShouldAcceptGesture: %d -> 1\n",
                 handler_id);
  }
  // No JS gesture arbitration in the replay; accept the Flutter recognizer.
  return 1;
}

SKYLINE_EXPORT extern "C" int32_t ShouldGestureResponse(
    void* /*runtime*/, int32_t handler_id, void* /*native_event*/) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] ShouldGestureResponse: %d -> 1\n",
                 handler_id);
  }
  return 1;
}

SKYLINE_EXPORT extern "C" uint8_t LayoutParagraphHitTestChildren(...) {
  return 0;
}

SKYLINE_EXPORT extern "C" SkylineLayoutSize LayoutParagraphPerformLayout(...) {
  return SkylineLayoutSize{0.0, 0.0};
}

SKYLINE_EXPORT extern "C" SkylineDryLayoutResult YogaCalculateDryLayout(
    intptr_t /*nodeAddress*/, double maxWidth, double maxHeight) {
#ifdef SKYLINE_EMBEDDED
  return SkylineDryLayoutResult{static_cast<float>(maxWidth),
                                static_cast<float>(maxHeight)};
#else
  return SkylineDryLayoutResult{0.0f, 0.0f};
#endif
}

SKYLINE_EXPORT extern "C" void YogaCalculateLayout(intptr_t /*nodeAddress*/,
                                                     double maxWidth,
                                                     double maxHeight) {
#ifdef SKYLINE_EMBEDDED
  if (g_json_active) {
    LayoutJsonNode(g_json_root, 0.0f, 0.0f, static_cast<float>(maxWidth),
                   static_cast<float>(maxHeight));
    return;
  }
  if (g_demo_nodes.empty()) {
    return;
  }
  const size_t child_count = g_demo_nodes.size() - 1;
  const float stripe_height =
      child_count == 0 ? static_cast<float>(maxHeight)
                       : static_cast<float>(maxHeight) / child_count;
  const float inset = 20.0f;
  const float text_width =
      static_cast<float>(maxWidth) - 2 * inset > 0 ? static_cast<float>(maxWidth) - 2 * inset : 0.0f;
  const float text_height =
      static_cast<float>(maxHeight) - 2 * inset > 0 ? static_cast<float>(maxHeight) - 2 * inset : 0.0f;
  for (const DemoNode& node : g_demo_nodes) {
    const auto found = g_yoga_metrics.find(node.address);
    if (found == g_yoga_metrics.end() || found->second == nullptr) {
      continue;
    }
    float* metrics = found->second;
    // [top, left, width, height, zIndex, isLayoutDirty, displayNone,
    //  marginBlock, marginInline] -- see package:skyline/widgets/yoga.dart.
    if (node.index == 0) {
      metrics[0] = 0.0f;
      metrics[1] = 0.0f;
      metrics[2] = static_cast<float>(maxWidth);
      metrics[3] = static_cast<float>(maxHeight);
    } else if (g_demo_mode == DemoMode::kText) {
      metrics[0] = inset;
      metrics[1] = inset;
      metrics[2] = text_width;
      metrics[3] = text_height;
    } else if (g_demo_mode == DemoMode::kCard) {
      constexpr float card_inset = 40.0f;
      const float card_width =
          static_cast<float>(maxWidth) - 2 * card_inset > 0
              ? static_cast<float>(maxWidth) - 2 * card_inset
              : 0.0f;
      const float card_height =
          static_cast<float>(maxHeight) - 2 * card_inset > 0
              ? static_cast<float>(maxHeight) - 2 * card_inset
              : 0.0f;
      metrics[0] = card_inset;
      metrics[1] = card_inset;
      metrics[2] = card_width;
      metrics[3] = card_height;
    } else {
      metrics[0] = static_cast<float>(node.index - 1) * stripe_height;
      metrics[1] = 0.0f;
      metrics[2] = static_cast<float>(maxWidth);
      metrics[3] = stripe_height;
    }
    metrics[4] = 0.0f;
    metrics[5] = 0.0f;
    metrics[6] = 0.0f;
    metrics[7] = 0.0f;
    metrics[8] = 0.0f;
  }
#endif
}

SKYLINE_EXPORT extern "C" SkylineNativeRect HandleHeroOnFrame(...) {
  return SkylineNativeRect{0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
}

// Minimal Linux stand-in for the private `skyline.node` runtime.
//
// `package:skyline` opens `libskyline.so` (or the path passed as
// `skylineAddonPath`) and resolves the 73 symbols below.  This library exports
// the same ABI so the recompiled appservice kernel can get through
// `initSkylineForDevtools` and render its Flutter UI.  Layout, paint, gestures
// and platform-channel behaviours are intentionally stubs; they are the next
// phase-3 milestones.
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
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

#ifdef SKYLINE_EMBEDDED
Dart_Isolate g_isolate = nullptr;
#endif

#ifdef SKYLINE_EMBEDDED
using SkylineHandle = Dart_Handle;

void LogTwo(const char* level, SkylineHandle tag, SkylineHandle message) {
  std::fprintf(stderr, "[skyline-runtime:%s] ", level);
  if (tag != nullptr && Dart_IsString(tag)) {
    const char* text = nullptr;
    if (Dart_StringToCString(tag, &text) == nullptr && text != nullptr) {
      std::fprintf(stderr, "%s: ", text);
    }
  }
  if (message != nullptr && Dart_IsString(message)) {
    const char* text = nullptr;
    if (Dart_StringToCString(message, &text) == nullptr && text != nullptr) {
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

SKYLINE_EXPORT extern "C" intptr_t CreateWindow(...) {
  return AsHandle(new Window());
}

SKYLINE_EXPORT extern "C" intptr_t CreateEmptyWindow(...) {
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
  if (std::getenv("SKYLINE_STUB_PAGE") == nullptr) {
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
                             int maxLines) {
  Dart_Handle text_handle = Dart_NewStringFromCString(value);
  Dart_Handle no_children = Dart_NewList(0);
  if (Dart_IsError(text_handle) || Dart_IsError(no_children)) {
    return Dart_IsError(text_handle) ? text_handle : no_children;
  }
  const double nan = std::nan("");
  Dart_Handle span = newTextSpan(
      address, text_handle, color, 0, 0, 0, 0, 0.0, nullptr, 0, fontSize,
      static_cast<uint8_t>(fontWeight), static_cast<uint8_t>(fontStyle),
      Dart_Null(), std::isnan(lineHeight) ? nan : lineHeight,
      std::isnan(letterSpacing) ? nan : letterSpacing, nan, 0, Dart_Null(),
      no_children);
  if (Dart_IsError(span)) {
    return span;
  }
  Dart_Handle spans = Dart_NewList(1);
  Dart_ListSetAt(spans, 0, span);
  return newText(address, reinterpret_cast<void*>(&DemoMarkDirty),
                 reinterpret_cast<void*>(&DemoCreateParagraph),
                 static_cast<uint8_t>(textAlign), 1 /* ltr */, 0 /* clip */,
                 1 /* softWrap */, static_cast<uint32_t>(maxLines), 0, 0,
                 0 /* Flutter text */, spans);
}

bool ReadDescriptor(const char* descriptor, std::string* out) {
  if (descriptor[0] != '@') {
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
// Feature toggles
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" uint8_t GetFeatureToggle(const char* feature) {
  // Every Skyline feature toggle defaults to false; the Dart side then uses its
  // documented default behaviour (e.g. Dart-side borders, no anchor refactor).
  (void)feature;
  return 0;
}

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
SKYLINE_VOID_STUB(ForwardHttpGet)
SKYLINE_VOID_STUB(HandleGestureEvent)
SKYLINE_VOID_STUB(HandleMouseEvent)
SKYLINE_VOID_STUB(HandleNativeViewPointerEvent)
SKYLINE_VOID_STUB(HandlePageRoute)
SKYLINE_VOID_STUB(HandlePageRouteDone)
SKYLINE_VOID_STUB(HandleTouchEvent)
SKYLINE_VOID_STUB(InitializeUITaskRunner)
SKYLINE_VOID_STUB(InstallJSEngineFromDart)
SKYLINE_VOID_STUB(LayoutParagraphMarkNeedsLayout)
SKYLINE_VOID_STUB(LayoutParagraphPaint)
SKYLINE_VOID_STUB(LoadResourceAsync)
SKYLINE_VOID_STUB(NotifyFontFaceLoad)
SKYLINE_VOID_STUB(NotifyNavigateBackByGesture)
SKYLINE_VOID_STUB(NotifyNavigateBackDoneByGesture)
SKYLINE_VOID_STUB(NotifyUserGestureInProgressChange)
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
SKYLINE_PTR_STUB(LoadResourceSync)
SKYLINE_PTR_STUB(ScrollViewMountChild)
// clang-format on

SKYLINE_EXPORT extern "C" void* GetSplashCacheImage(...) {
  return nullptr;  // no splash cache in the stub runtime
}

// ---------------------------------------------------------------------------
// Scalar / struct returns
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" double AdjustDecelerationVelocity(...) {
  return 0.0;
}

SKYLINE_EXPORT extern "C" int32_t ShouldAcceptGesture(...) {
  return 0;
}

SKYLINE_EXPORT extern "C" int32_t ShouldGestureResponse(...) {
  return 0;
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

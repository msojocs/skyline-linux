// Linux stand-in for the private `skyline.node` runtime: ABI entry TU.
//
// `package:skyline` opens `libskyline.so` (or the path passed as
// `skylineAddonPath`) and resolves the 73 symbols below.  This translation unit
// owns the bootstrap/ABI surface and the exported no-op stubs shared by both the
// standalone Dart-API-DL library and the engine's embedded build.  The embedded
// replay (demo pages, CSS, layout, widgets, builders, operations, routes and
// platform channels) lives in the `appservice_*.cc` modules; see
// `native/runtime/sources.txt`.  Remaining gaps are listed in
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
#include "internal/skyline_runtime_module.hh"

// Fingerprint of the runtime sources compiled into this binary.  The engine
// build (native/engine/build-engine.sh) generates the header next to the
// copied sources; standalone CMake builds are "unversioned".
#if __has_include("skyline_runtime_build_id.hh")
#include "skyline_runtime_build_id.hh"
#endif
#ifndef SKYLINE_RUNTIME_BUILD_ID
#define SKYLINE_RUNTIME_BUILD_ID "unversioned"
#endif


// ABI/bootstrap state.  Declared in internal/skyline_runtime_abi.hh.
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

bool g_dart_api_initialized = false;
double g_viewport_width = 0.0;
double g_viewport_height = 0.0;

#ifdef SKYLINE_EMBEDDED
Dart_Isolate g_isolate = nullptr;

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
// The Dart API DL table does not expose string conversion, so log lines only
// carry the level.  Message text is still visible through
// `--entrypoint-arg` diagnostics / the Flutter host's own log callback.
void LogTwo(const char* level, SkylineHandle /*tag*/, SkylineHandle /*message*/) {
  std::fprintf(stderr, "[skyline-runtime:%s] <message>\n", level);
}
#endif

// ---------------------------------------------------------------------------
// Bootstrap / runtime objects
// ---------------------------------------------------------------------------

// Returns "SKYLINE_RUNTIME_BUILD_ID=<sha256>"; tools/build-status.js finds the
// same prefixed string in the binary to detect engines with a stale runtime.
SKYLINE_EXPORT extern "C" const char* SkylineRuntimeBuildId() {
  return "SKYLINE_RUNTIME_BUILD_ID=" SKYLINE_RUNTIME_BUILD_ID;
}

SKYLINE_EXPORT extern "C" intptr_t InitDartAPIDLSkyline(void* data) {
#ifdef SKYLINE_EMBEDDED
  // The full Dart API is linked in; nothing to initialize.
  (void)data;
  g_dart_api_initialized = true;
  std::fprintf(stderr, "[skyline-runtime] embedded Dart API ready (build %s)\n",
               SKYLINE_RUNTIME_BUILD_ID);
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
    void* /*runtime_ptr*/, int32_t id, void* /*ffi_table_ptr*/,
    double window_width, double window_height, double /*padding_top*/,
    double /*padding_right*/, double /*padding_bottom*/,
    double /*padding_left*/, double /*device_pixel_ratio*/,
    void* /*window_holder*/) {
  g_viewport_width = window_width;
  g_viewport_height = window_height;
  auto* window = new Window();
  window->id = id;
  return AsHandle(window);
}

SKYLINE_EXPORT extern "C" intptr_t CreateEmptyWindow(
    void* /*runtime_ptr*/, int32_t id, void* /*ffi_table_ptr*/,
    void* /*window_holder*/) {
  auto* window = new Window();
  window->id = id;
  return AsHandle(window);
}

SKYLINE_EXPORT extern "C" void SetCurrentDartIsolate(void* holder_ptr) {
  RuntimeHolder* holder = HolderOrFallback(holder_ptr);
  holder->dart_isolate = holder;
#ifdef SKYLINE_EMBEDDED
  g_isolate = Dart_CurrentIsolate();
#endif
}

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

#ifdef SKYLINE_EMBEDDED
void CreateDemoPage(RuntimeHolder* holder);
void CreateTextPage(RuntimeHolder* holder);
void CreateJsonPage(RuntimeHolder* holder, const char* descriptor);
void CreateAppServiceOpsPage(RuntimeHolder* holder, const char* descriptor);
void ScheduleCreateDemoPage(RuntimeHolder* holder);
#endif

// Defined with the live DevTools page bridge at the end of this file.
void ApplyPendingLiveOpsOnUi();

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
    ApplyPendingLiveOpsOnUi();
#endif
  } else {
    std::fprintf(stderr, "[skyline-runtime] NotifyFlutterReady: RunApplication not registered\n");
  }
}


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
  return config::Flag(config::kAppServiceDebug);
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

// Layout mirrors `NativePointerEvent` in package:skyline/gestures/touch_event.dart.
struct NativePointerEventLayout {
  int32_t embedder_id;
  int64_t time_stamp_microseconds;
  int32_t pointer;
  int32_t kind;
  int32_t device;
  double position_x;
  double position_y;
  double delta_x;
  double delta_y;
  int32_t buttons;
  uint8_t down;
  uint8_t obscured;
  double pressure;
  double pressure_min;
  double pressure_max;
  double distance;
  double distance_max;
  double size;
  double radius_major;
  double radius_minor;
  double radius_min;
  double radius_max;
  double orientation;
  double tilt;
  int32_t platform_data;
  uint8_t synthesized;
  uint8_t type;
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

#ifdef SKYLINE_EMBEDDED
SKYLINE_EXPORT extern "C" void HandleNativeViewPointerEvent(
    void* /*runtime_ptr*/, int64_t view_id, void* event) {
  if (event == nullptr) return;
  const auto* pointer = static_cast<const NativePointerEventLayout*>(event);
  appservice::AppNode* node = nullptr;
  auto found = appservice::g_texture_nodes.find(static_cast<int>(view_id));
  if (found != appservice::g_texture_nodes.end()) node = found->second;
  if (!DebugChannels()) return;
  static const char* const kTypes[] = {"down", "move", "up", "cancel"};
  const char* type =
      pointer->type < 4 ? kTypes[pointer->type] : "unknown";
  std::fprintf(stderr,
               "[skyline-runtime] HandleNativeViewPointerEvent: viewId=%ld "
               "node=%u type=%s pointer=%d kind=%d down=%u pos=(%g,%g) "
               "delta=(%g,%g) pressure=%g time=%ld\n",
               static_cast<long>(view_id), node != nullptr ? node->id : 0u,
               type, pointer->pointer, pointer->kind,
               static_cast<unsigned>(pointer->down), pointer->position_x,
               pointer->position_y, pointer->delta_x, pointer->delta_y,
               pointer->pressure,
               static_cast<long>(pointer->time_stamp_microseconds));
}
#else
// Native views only exist in the embedded runtime; keep the export for the
// standalone DL-only library.
SKYLINE_EXPORT extern "C" void HandleNativeViewPointerEvent(
    void* /*runtime_ptr*/, int64_t /*view_id*/, void* /*event*/) {}
#endif

SKYLINE_EXPORT extern "C" void HandlePageRoute(void* /*runtime_ptr*/,
                                               void* name, double duration,
                                               uint8_t route_type) {
  const char* text = DartStringOrNull(static_cast<SkylineHandle>(name));
  if (text == nullptr || text[0] == '\0') return;
  if (DebugChannels()) {
    std::fprintf(
        stderr, "[skyline-runtime] HandlePageRoute: name=%s duration=%g type=%u\n",
        text, duration, static_cast<unsigned>(route_type));
  }
  // The custom-route builder registered a worklet for `<eventName>-<viewTag>`
  // (`onAnimationChange-…` / `onSecondaryAnimationChange-…`, with a `status-`
  // prefix for status changes).  Forward the controller value so the JS route
  // can drive its animated styles.  (The DL-only library has no appservice
  // namespace/replay, so it only logs.)
#ifdef SKYLINE_EMBEDDED
  char tail[96];
  std::snprintf(tail, sizeof(tail), "\",\"value\":%.6f,\"status\":%u}",
                duration, static_cast<unsigned>(route_type));
  std::string json = "{\"type\":\"pageRoute\",\"name\":\"";
  json += appservice::JsonEscapeString(text);
  json += tail;
  appservice::EmitJsonEvent(json);
#endif
}

#ifdef SKYLINE_EMBEDDED
// Called by the Dart route when a gesture-driven pop starts/finishes.  The
// runtime's page stack mirrors what the Flutter route just did, and the shell
// is told so the DevTools simulator route stack follows (this is the
// `setNavigateBackCallback`/`setNavigateBackDoneCallback` contract).
bool PopPageEntryForGesture(int32_t page_id) {
  if (appservice::g_page == nullptr || appservice::g_page->page_stack.empty()) {
    return false;
  }
  if (page_id != 0 && appservice::g_page->current_page_id != 0 &&
      appservice::g_page->current_page_id != page_id) {
    return false;
  }
  appservice::DeletePersistent(&appservice::g_page->root_node);
  appservice::DeletePersistent(&appservice::g_page->root_connector);
  appservice::OpPage::PageEntry entry =
      appservice::g_page->page_stack.back();
  appservice::g_page->page_stack.pop_back();
  appservice::g_page->root_id = entry.root_id;
  appservice::g_page->root_node = entry.root_node;
  appservice::g_page->root_connector = entry.root_connector;
  appservice::g_page->current_page_id = entry.page_id;
  if (appservice::g_page->page_count > 0) appservice::g_page->page_count--;
  if (appservice::g_page->root_id != 0 &&
      appservice::g_page->root_connector != nullptr) {
    appservice::g_page->force_rebuild = true;
  }
  return true;
}
#endif

SKYLINE_EXPORT extern "C" void NotifyNavigateBackByGesture(
    void* /*runtime_ptr*/, void* window_ptr, int32_t page_id) {
  auto* window = static_cast<Window*>(window_ptr);
  const int32_t window_id = window != nullptr ? window->id : 0;
#ifdef SKYLINE_EMBEDDED
  const bool popped = PopPageEntryForGesture(page_id);
  std::fprintf(stderr,
               "[skyline-runtime] NotifyNavigateBackByGesture: window=%d "
               "page=%d popped=%d\n",
               window_id, page_id, popped ? 1 : 0);
  char buffer[128];
  std::snprintf(
      buffer, sizeof(buffer),
      "{\"type\":\"navigateBackByGesture\",\"windowId\":%d,\"pageId\":%d}",
      window_id, page_id);
  appservice::EmitJsonEvent(buffer);
#else
  std::fprintf(stderr,
               "[skyline-runtime] NotifyNavigateBackByGesture: window=%d "
               "page=%d\n",
               window_id, page_id);
#endif
}

SKYLINE_EXPORT extern "C" void NotifyNavigateBackDoneByGesture(
    void* /*runtime_ptr*/, void* window_ptr, int32_t page_id) {
  auto* window = static_cast<Window*>(window_ptr);
  const int32_t window_id = window != nullptr ? window->id : 0;
  std::fprintf(stderr,
               "[skyline-runtime] NotifyNavigateBackDoneByGesture: window=%d "
               "page=%d\n",
               window_id, page_id);
#ifdef SKYLINE_EMBEDDED
  char buffer[128];
  std::snprintf(
      buffer, sizeof(buffer),
      "{\"type\":\"navigateBackDoneByGesture\",\"windowId\":%d,\"pageId\":%d}",
      window_id, page_id);
  appservice::EmitJsonEvent(buffer);
#endif
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

// ---------------------------------------------------------------------------
// Generic void exports (arguments ignored)
// ---------------------------------------------------------------------------

#define SKYLINE_VOID_STUB(name) SKYLINE_EXPORT extern "C" void name(...) {}

// clang-format off
SKYLINE_VOID_STUB(BorderPaint)
SKYLINE_VOID_STUB(DidChangeAppLifecycleState)
SKYLINE_VOID_STUB(DidHaveMemoryPressure)
SKYLINE_VOID_STUB(DoIDKeyReport)
SKYLINE_VOID_STUB(DoKVReport)
SKYLINE_VOID_STUB(EvaluateJavaScript)
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(AddPendingFont)
#endif
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(ForwardHttpGet)
#endif
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(HandleGestureEvent)
#endif
SKYLINE_VOID_STUB(InitializeUITaskRunner)
SKYLINE_VOID_STUB(InstallJSEngineFromDart)
SKYLINE_VOID_STUB(LayoutParagraphMarkNeedsLayout)
SKYLINE_VOID_STUB(LayoutParagraphPaint)
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(LoadResourceAsync)
SKYLINE_VOID_STUB(NotifyFontFaceLoad)
#endif
SKYLINE_VOID_STUB(NotifyWindowReady)
SKYLINE_VOID_STUB(OnLayoutFinished)
SKYLINE_VOID_STUB(OnViewportChanged)
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(RemovePendingFont)
#endif
SKYLINE_VOID_STUB(ReportFrameTiming)
SKYLINE_VOID_STUB(ReportPerformance)
#ifndef SKYLINE_EMBEDDED
SKYLINE_VOID_STUB(SceneToImage)
#endif
SKYLINE_VOID_STUB(SetDevicePixelRatio)
SKYLINE_VOID_STUB(TraceBeginDart)
SKYLINE_VOID_STUB(TraceEndDart)
SKYLINE_VOID_STUB(TraceFlushDart)
SKYLINE_VOID_STUB(TraceInstantDart)
SKYLINE_VOID_STUB(UpdatePlatformBrightness)
#ifndef SKYLINE_EMBEDDED
// Embedded builds provide the real implementations above (accessibility
// bridge / semantics tree).  The DL-only library still has to export the full
// 73-symbol lookup table.
SKYLINE_VOID_STUB(OnAccessibilityFeaturesChanged)
SKYLINE_VOID_STUB(UpdateAccessibility)
#endif
#ifndef SKYLINE_EMBEDDED
// Embedded builds provide the real implementations above; the DL-only stub
// still has to export the full 73-symbol lookup table.
SKYLINE_VOID_STUB(OnNavigateBackIntercept)
SKYLINE_VOID_STUB(YogaTriggerIntersection)
SKYLINE_VOID_STUB(YogaTriggerIntersectionObservers)
#endif
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
  if (config::Flag(config::kAppServiceDebug)) {
    std::fprintf(stderr, "[skyline-runtime] ShouldAcceptGesture: %d -> 1\n",
                 handler_id);
  }
  // `worklet:should-accept-gesture` is not replayed: accept the recognizer.
  return 1;
}

#ifdef SKYLINE_EMBEDDED
namespace {

// NativeOriginalPointerEvent layout
// (package:skyline/gestures/pointer_event.dart): int64 identifier, char* type,
// then double deltaX, deltaY, localX, localY, clientX, clientY, radiusX,
// radiusY, rotationAngle, force, tilt and an int64 timeStamp.
constexpr size_t kPointerDeltaXOffset = 16;
constexpr size_t kPointerDeltaYOffset = 24;

double ReadNativePointerDouble(const void* event, size_t offset) {
  double value = 0.0;
  std::memcpy(&value, static_cast<const char*>(event) + offset, sizeof(value));
  return value;
}

// The official renderer answers `worklet:should-response-on-move` from JS.
// The replay cannot call into the renderer bundle synchronously, so the common
// nested pattern is decided natively: a scroll view inside a swiper yields
// when it is already at the edge in the drag direction and the swiper still
// has a page there (the associated-scroll-view tab linkage).
bool ScrollViewYieldsToSwiper(int32_t handler_id, double dx, double dy) {
  if (appservice::g_page == nullptr) return false;
  appservice::AppNode* scroll =
      appservice::FindNode(static_cast<uint32_t>(handler_id));
  if (scroll == nullptr || scroll->released ||
      !appservice::IsScrollViewName(scroll->name)) {
    return false;
  }
  appservice::AppNode* swiper = nullptr;
  for (appservice::AppNode* parent = scroll->parent; parent != nullptr;
       parent = parent->parent) {
    if (appservice::IsSwiperNode(parent)) {
      swiper = parent;
      break;
    }
  }
  if (swiper == nullptr || swiper->children.empty()) return false;
  const bool swiper_vertical = appservice::AttrBool(swiper, "vertical", false);
  const bool scroll_vertical = appservice::AttrBool(scroll, "scrollY", false);
  if (swiper_vertical != scroll_vertical) return false;

  const double offset =
      swiper_vertical ? appservice::AttrNumber(scroll, "scrollTop", 0.0)
                      : appservice::AttrNumber(scroll, "scrollLeft", 0.0);
  const double total =
      swiper_vertical ? appservice::AttrNumber(scroll, "scrollHeight", 0.0)
                      : appservice::AttrNumber(scroll, "scrollWidth", 0.0);
  const double viewport =
      swiper_vertical ? scroll->measured_h : scroll->measured_w;
  const double delta = swiper_vertical ? dy : dx;
  if (delta == 0.0 || viewport <= 0.0) return false;
  // Before the first scroll event the content extent is unknown; the builtin
  // starts from `viewport + 1` and the user worklet treats it as not at the
  // bottom.  Respond so the list scrolls and publishes its real extent.
  if (total <= 0.0) return false;

  const bool scrollable = total > viewport + 0.5;
  const int current =
      static_cast<int>(appservice::AttrNumber(swiper, "current", 0.0));
  const int pages = static_cast<int>(swiper->children.size());
  const bool circular = appservice::AttrBool(swiper, "circular", false);
  const bool at_start = !scrollable || offset <= 0.5;
  const bool at_end = !scrollable || offset + viewport >= total - 0.5;
  const bool can_previous = circular || current > 0;
  const bool can_next = circular || current < pages - 1;
  const bool yield =
      (delta > 0.0 && at_start && can_previous) ||
      (delta < 0.0 && at_end && can_next);
  if (config::Flag(config::kAppServiceDebug)) {
    std::fprintf(stderr,
                 "[skyline-runtime] ShouldGestureResponse: scroll #%u -> "
                 "swiper #%u offset=%g total=%g viewport=%g delta=%g "
                 "page=%d/%d yield=%d\n",
                 scroll->id, swiper->id, offset, total, viewport, delta,
                 current, pages, yield ? 1 : 0);
  }
  return yield;
}

}  // namespace
#endif  // SKYLINE_EMBEDDED

SKYLINE_EXPORT extern "C" int32_t ShouldGestureResponse(
    void* /*runtime*/, int32_t handler_id, void* native_event) {
#ifdef SKYLINE_EMBEDDED
  if (native_event != nullptr) {
    const double dx =
        ReadNativePointerDouble(native_event, kPointerDeltaXOffset);
    const double dy =
        ReadNativePointerDouble(native_event, kPointerDeltaYOffset);
    if (ScrollViewYieldsToSwiper(handler_id, dx, dy)) return 0;
  }
#else
  (void)handler_id;
  (void)native_event;
#endif
  if (config::Flag(config::kAppServiceDebug)) {
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

namespace {

void AppendHeroRect(std::string* json, const char* name,
                    const SkylineNativeRect* rect) {
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;
  double width = 0.0;
  double height = 0.0;
  if (rect != nullptr) {
    left = rect->left;
    top = rect->top;
    right = rect->right;
    bottom = rect->bottom;
    width = rect->width;
    height = rect->height;
  }
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "\"%s\":{\"left\":%.4f,\"top\":%.4f,\"right\":%.4f,"
                "\"bottom\":%.4f,\"width\":%.4f,\"height\":%.4f}",
                name, left, top, right, bottom, width, height);
  *json += buffer;
}

}  // namespace

// The synthetic-page layout callback lives with the demo pages (embedded
// build).  The DL-only library still exports the lookup symbol.
#ifndef SKYLINE_EMBEDDED
SKYLINE_EXPORT extern "C" void YogaCalculateLayout(intptr_t /*nodeAddress*/,
                                                   double /*maxWidth*/,
                                                   double /*maxHeight*/) {}
#endif

// Dart's `SkylineHero` calls this while a route transition (push, pop or a
// user-gesture back swipe) animates the shared element.  The builtin
// share-element component registered its `worklet:onframe` handler as
// `onFrame-<viewTag>`; forward the frame data to JS so app code can drive
// shared values (card scale/opacity, the route's source/destination rects).
// The shuttle rect is returned unchanged: the JS worklet only mutates shared
// values, it does not choose the geometry.
SKYLINE_EXPORT extern "C" SkylineNativeRect HandleHeroOnFrame(
    intptr_t /*runtime_ptr*/, SkylineHandle event_hash,
    const SkylineNativeRect* begin, const SkylineNativeRect* end,
    const SkylineNativeRect* current, double progress, int32_t direction) {
  SkylineNativeRect result =
      current != nullptr ? *current
                         : SkylineNativeRect{0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
#ifdef SKYLINE_EMBEDDED
  if (event_hash == nullptr || Dart_IsError(event_hash) ||
      Dart_IsNull(event_hash)) {
    return result;
  }
  const char* hash = nullptr;
  if (Dart_IsError(Dart_StringToCString(event_hash, &hash)) ||
      hash == nullptr || *hash == '\0') {
    return result;
  }
  std::string json;
  json.reserve(320);
  json += "{\"type\":\"heroFrame\",\"hash\":\"";
  json += appservice::JsonEscapeString(hash);
  if (config::Flag(config::kAppServiceDebug)) {
    std::fprintf(stderr,
                 "[skyline-runtime] hero frame hash=%s progress=%.3f "
                 "direction=%d\n",
                 hash, progress, static_cast<int>(direction));
  }
  char head[96];
  std::snprintf(head, sizeof(head), "\",\"direction\":%d,\"progress\":%.6f,",
                static_cast<int>(direction), progress);
  json += head;
  AppendHeroRect(&json, "begin", begin);
  json += ',';
  AppendHeroRect(&json, "end", end);
  json += ',';
  AppendHeroRect(&json, "current", current);
  json += '}';
  appservice::EmitJsonEvent(json);
#else
  (void)event_hash;
  (void)begin;
  (void)end;
  (void)progress;
  (void)direction;
#endif
  return result;
}

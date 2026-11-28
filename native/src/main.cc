// skyline.node: the N-API module the WeChat DevTools loads as the Skyline
// addon.  Loading it runs the bootstrap (src/bootstrap, which defines
// SkylineShell and friends) against the `native.*` bindings below; window
// lifecycle, input and frames go through the WindowBackend, everything else
// calls the embedded runtime (skyline/runtime_api.h) directly.

#include <node_api.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "bootstrap_js.h"
#include "napi_binding.hh"
#include "window_backend.hh"
#ifdef SKYLINE_HAVE_ENGINE
#include "flutter_window_backend.hh"
#else
// No engine and therefore no runtime is linked: the calls below stay
// type-checked but are never made, and the weak references stay null.
#define SKYLINE_RUNTIME_API_WEAK
#endif
#include "skyline/runtime_api.h"
#include "skyline/config.h"

// RUNTIME(call) calls into the embedded runtime; RUNTIME_OR(call, fallback)
// yields `fallback` when skyline.node is built without an engine.
#ifdef SKYLINE_HAVE_ENGINE
#define RUNTIME(call) (call)
#define RUNTIME_OR(call, fallback) (call)
#else
#define RUNTIME(call) (false ? (call) : void())
#define RUNTIME_OR(call, fallback) (false ? (call) : (fallback))
#endif

namespace {

using skyline::napi::Args;
using skyline::napi::Binding;
using skyline::napi::Error;
using skyline::napi::NewObject;
using skyline::napi::SetNumber;
using skyline::napi::ThreadsafeSink;

void AddonLog(const char* format, ...);

// -- Events delivered to the bootstrap ------------------------------------------

struct ScrollEvent {
  uint32_t node_id = 0;
  int32_t phase = 0;
  double scroll_top = 0.0;
  double scroll_left = 0.0;
  double scroll_width = 0.0;
  double scroll_height = 0.0;
  double delta_x = 0.0;
  double delta_y = 0.0;
  bool is_drag = false;
  double boundary_velocity = 0.0;
};

struct WidgetEvent {
  uint32_t node_id = 0;
  int32_t kind = 0;
  double value = 0.0;
  double extra = 0.0;
  uint8_t flag = 0;
};

struct JsonEvent {
  std::string json;
};

struct HttpRequestEvent {
  uint32_t request_id = 0;
  std::string url;
};

struct FrameEvent {
  double window_id = 0.0;
};

// The event object the bootstrap turns into `bindscroll` / worklet callbacks.
napi_value ScrollEventToJs(napi_env env, const ScrollEvent& event) {
  napi_value object = NewObject(env);
  SetNumber(env, object, "nodeId", static_cast<double>(event.node_id));
  SetNumber(env, object, "phase", static_cast<double>(event.phase));
  SetNumber(env, object, "scrollTop", event.scroll_top);
  SetNumber(env, object, "scrollLeft", event.scroll_left);
  SetNumber(env, object, "scrollWidth", event.scroll_width);
  SetNumber(env, object, "scrollHeight", event.scroll_height);
  SetNumber(env, object, "deltaX", event.delta_x);
  SetNumber(env, object, "deltaY", event.delta_y);
  SetNumber(env, object, "boundaryVelocity", event.boundary_velocity);
  skyline::napi::SetBool(env, object, "isDrag", event.is_drag);
  const char* kind = event.phase == SKYLINE_SCROLL_PHASE_START ? "scrollStart"
                     : event.phase == SKYLINE_SCROLL_PHASE_END ? "scrollEnd"
                                                               : "scroll";
  skyline::napi::SetString(env, object, "kind", kind);
  return object;
}

// Routed by the bootstrap to the builtin swiper / picker-view mirror.
napi_value WidgetEventToJs(napi_env env, const WidgetEvent& event) {
  napi_value object = NewObject(env);
  SetNumber(env, object, "nodeId", static_cast<double>(event.node_id));
  SetNumber(env, object, "kind", static_cast<double>(event.kind));
  SetNumber(env, object, "value", event.value);
  SetNumber(env, object, "extra", event.extra);
  SetNumber(env, object, "flag", static_cast<double>(event.flag));
  return object;
}

napi_value JsonEventToJs(napi_env env, const JsonEvent& event) {
  napi_value json = nullptr;
  skyline::napi::Check(napi_create_string_utf8(env, event.json.c_str(),
                                               NAPI_AUTO_LENGTH, &json),
                       "napi_create_string_utf8");
  return json;
}

napi_value HttpRequestToJs(napi_env env, const HttpRequestEvent& event) {
  napi_value object = NewObject(env);
  napi_value request_id = nullptr;
  skyline::napi::Check(napi_create_uint32(env, event.request_id, &request_id),
                       "napi_create_uint32");
  skyline::napi::Check(
      napi_set_named_property(env, object, "requestId", request_id),
      "napi_set_named_property");
  skyline::napi::SetString(env, object, "url", event.url.c_str());
  return object;
}

// The DevTools waits for `{window_id}` to forward SKYLINE_WINDOW_RASTERIZED to
// the pageframe; a bare invocation makes `e.window_id` throw and the
// pageframe never pulls the new frame.
napi_value FrameEventToJs(napi_env env, const FrameEvent& event) {
  napi_value object = NewObject(env);
  SetNumber(env, object, "window_id", event.window_id);
  return object;
}

using ScrollSink = ThreadsafeSink<ScrollEvent, &ScrollEventToJs>;
using WidgetSink = ThreadsafeSink<WidgetEvent, &WidgetEventToJs>;
using JsonSink = ThreadsafeSink<JsonEvent, &JsonEventToJs>;
using HttpRequestSink = ThreadsafeSink<HttpRequestEvent, &HttpRequestToJs>;
using FrameSink = ThreadsafeSink<FrameEvent, &FrameEventToJs>;

// The runtime's sinks are plain function pointers, so their JS ends are
// process-wide.  They never keep the Node loop alive: tests load the addon,
// create a window and expect the loop to drain.  Never destroyed, so engine
// threads cannot outlive them.
ScrollSink& ScrollEvents() {
  static auto* sink = new ScrollSink("skyline_scroll", "scroll event", 32, false);
  return *sink;
}
WidgetSink& WidgetEvents() {
  static auto* sink = new WidgetSink("skyline_widget", "widget event", 32, false);
  return *sink;
}
JsonSink& JsonEvents() {
  static auto* sink = new JsonSink("skyline_json_event", "JSON event", 64, false);
  return *sink;
}
HttpRequestSink& HttpRequests() {
  static auto* sink =
      new HttpRequestSink("skyline_http_request", "HTTP request", 32, false);
  return *sink;
}

// Set when the DevTools pageframe registers SkylineShell.setHttpRequestCallback.
std::atomic<bool> g_http_request_callback_registered{false};

// Engine UI thread: only queue; the JS thread builds the event objects.
void OnScrollEvent(uint32_t node_id, int32_t phase, double scroll_top,
                   double scroll_left, double scroll_width,
                   double scroll_height, double delta_x, double delta_y,
                   uint8_t is_drag, double boundary_velocity) {
  ScrollEvents().Post(ScrollEvent{node_id, phase, scroll_top, scroll_left,
                                  scroll_width, scroll_height, delta_x,
                                  delta_y, is_drag != 0, boundary_velocity});
}

void OnWidgetEvent(uint32_t node_id, int32_t kind, double value, double extra,
                   uint8_t flag) {
  WidgetEvents().Post(WidgetEvent{node_id, kind, value, extra, flag});
}

void OnJsonEvent(const char* json) {
  if (json != nullptr) JsonEvents().Post(JsonEvent{json});
}

void OnHttpRequest(uint32_t request_id, const char* url) {
  HttpRequests().Post(HttpRequestEvent{request_id, url != nullptr ? url : ""});
}

int32_t HasHttpRequestCallback() {
  return g_http_request_callback_registered.load() ? 1 : 0;
}

// -- Module state ------------------------------------------------------------------

struct AddonState {
  std::unique_ptr<skyline::WindowBackend> backend;
  // One rasterized-callback sink per window.  A present that started before
  // a replacement may still post to the old sink, so the backend callback
  // holds its own reference and a replaced sink is closed, not just dropped.
  std::map<double, std::shared_ptr<FrameSink>> frame_sinks;
};

AddonState& State(napi_env env) {
  void* data = nullptr;
  if (napi_get_instance_data(env, &data) != napi_ok || data == nullptr) {
    throw Error("Addon state is unavailable");
  }
  return *static_cast<AddonState*>(data);
}

// Environment teardown: no event may reach the JS thread afterwards.
void CleanupCallbacks(void* argument) {
  auto* state = static_cast<AddonState*>(argument);
  if (state == nullptr) return;
  // Stop engine threads before the environment and shared-memory views go
  // away; DestroyWindow joins each window's platform thread.
  state->backend.reset();
  RUNTIME(SkylineSetScrollEventSink(nullptr));
  RUNTIME(SkylineSetWidgetEventSink(nullptr));
  RUNTIME(SkylineSetJsonEventSink(nullptr));
  RUNTIME(SkylineSetHttpRequestSink(nullptr, nullptr));
  ScrollEvents().Close(napi_tsfn_abort);
  WidgetEvents().Close(napi_tsfn_abort);
  JsonEvents().Close(napi_tsfn_abort);
  HttpRequests().Close(napi_tsfn_abort);
  for (auto& [window_id, sink] : state->frame_sinks) {
    (void)window_id;
    sink->Close(napi_tsfn_abort);
  }
  state->frame_sinks.clear();
}

void FinalizeState(napi_env /*env*/, void* data, void* /*hint*/) {
  auto* state = static_cast<AddonState*>(data);
  if (state == nullptr) return;
  // Window threads must be joined before the module state disappears.
  state->backend.reset();
  delete state;
}

// -- Windows -----------------------------------------------------------------------

// The official Windows ABI passes a 16-byte handle (LE64 pointer, LE64 size)
// for Boost shared memory.  On Linux the same field is accepted when it points
// at process-local shared memory.
bool ReadSharedMemoryHandle(napi_env env, napi_value value, void** data,
                            size_t* byte_length) {
  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) != napi_ok ||
      !is_typed_array) {
    return false;
  }
  void* bytes = nullptr;
  size_t length = 0;
  if (!skyline::napi::ReadBufferView(env, value, &bytes, &length) ||
      length != 16) {
    return false;
  }
  const auto* raw = static_cast<const uint8_t*>(bytes);
  uint64_t pointer = 0;
  uint64_t size = 0;
  for (int i = 0; i < 8; ++i) {
    pointer |= static_cast<uint64_t>(raw[i]) << (i * 8);
    size |= static_cast<uint64_t>(raw[8 + i]) << (i * 8);
  }
  if (pointer == 0 || size < 8) return false;
  *data = reinterpret_cast<void*>(static_cast<uintptr_t>(pointer));
  *byte_length = static_cast<size_t>(size);
  return true;
}

// The bootstrap passes the SkylineShell.createWindow arguments as one array.
skyline::CreateWindowArgs ParseCreateWindow(napi_env env, napi_value array) {
  napi_value items[9] = {};
  for (uint32_t i = 0; i < 9; ++i) {
    if (napi_get_element(env, array, i, &items[i]) != napi_ok) {
      items[i] = nullptr;
    }
  }
  auto is = [env](napi_value value, napi_valuetype type) {
    napi_valuetype actual = napi_undefined;
    return value != nullptr && napi_typeof(env, value, &actual) == napi_ok &&
           actual == type;
  };
  auto number = [env](napi_value value) {
    double result = 0.0;
    napi_get_value_double(env, value, &result);
    return result;
  };
  skyline::CreateWindowArgs args;
  if (is(items[0], napi_number)) args.window_id = number(items[0]);
  if (is(items[1], napi_string)) {
    args.bundle_path = skyline::napi::ToString(env, items[1]);
  }
  if (is(items[2], napi_number)) args.width = number(items[2]);
  if (is(items[3], napi_number)) args.height = number(items[3]);
  if (is(items[4], napi_number)) args.device_pixel_ratio = number(items[4]);
  if (is(items[5], napi_boolean)) {
    bool hide = false;
    napi_get_value_bool(env, items[5], &hide);
    args.hide_window = hide;
  }
  if (is(items[7], napi_string)) {
    args.skyline_addon_path = skyline::napi::ToString(env, items[7]);
  }
  void* data = nullptr;
  size_t length = 0;
  // 6th argument: Linux shared-memory data area (ArrayBuffer / typed array).
  if (items[6] != nullptr &&
      skyline::napi::ReadBufferView(env, items[6], &data, &length)) {
    args.frame_buffer = data;
    args.frame_buffer_size = length;
  }
  // 9th argument: Windows 16-byte descriptor, or the live Linux ArrayBuffer
  // that the DevTools' sharedMemory.setMemory() returns.  A descriptor that
  // does not decode is ignored; a live view is only used when the 6th argument
  // did not already provide the frame buffer.
  if (items[8] != nullptr) {
    if (ReadSharedMemoryHandle(env, items[8], &data, &length)) {
      args.frame_buffer = data;
      args.frame_buffer_size = length;
    } else if (args.frame_buffer == nullptr &&
               skyline::napi::ReadBufferView(env, items[8], &data, &length)) {
      args.frame_buffer = data;
      args.frame_buffer_size = length;
    }
  }
  return args;
}

napi_value CreateWindow(const Args& args) {
  if (!args.Is(0, napi_object)) {
    throw Error("createWindow expects an arguments array");
  }
  AddonState& state = State(args.env());
  if (!state.backend) throw Error("Skyline engine is unavailable");
  const skyline::CreateWindowArgs window =
      ParseCreateWindow(args.env(), args[0]);
  AddonLog("createWindow id=%g bundle=%s addon=%s buffer=%p size=%zu",
           window.window_id, window.bundle_path.c_str(),
           window.skyline_addon_path.c_str(), window.frame_buffer,
           window.frame_buffer_size);
  state.backend->CreateWindow(window);
  // DevTools integration demo: render a static page while the private
  // appservice JS bridge is unavailable.  The page ops use the same schema as
  // the recorded fixtures.
  const std::string& demo =
      skyline::config::String(skyline::config::kDevtoolsDemoOps);
  if (!demo.empty()) {
    std::ifstream file(demo);
    if (file) {
      const std::string json((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
      if (!json.empty()) {
        RUNTIME(SkylineApplyLiveOps(json.c_str(), window.window_id));
      }
    }
  }
  return nullptr;
}

napi_value DestroyWindow(const Args& args) {
  AddonState& state = State(args.env());
  if (!state.backend) return nullptr;
  const double window_id = args.Number(0);
  state.backend->DestroyWindow(window_id);
  // The engine threads are joined, so no present can post any more.
  auto sink = state.frame_sinks.find(window_id);
  if (sink != state.frame_sinks.end()) {
    sink->second->Close();
    state.frame_sinks.erase(sink);
  }
  return nullptr;
}

napi_value OnWindowRasterized(const Args& args) {
  AddonState& state = State(args.env());
  if (!state.backend) throw Error("Skyline engine is unavailable");
  const double window_id = args.Number(0);
  if (!state.backend->HasWindow(window_id)) {
    throw Error("window_id: " +
                std::to_string(static_cast<long long>(window_id)) +
                " not found");
  }
  if (!args.Is(1, napi_function)) throw Error("Callback must be a function");
  // Frame callbacks are wake-ups, not data: a queue of one coalesces bursts
  // while the Node event loop is busy.  The sink keeps the loop alive until
  // the window is destroyed.
  auto sink = std::make_shared<FrameSink>("skyline_frame", "frame", 1, true);
  sink->Install(args.env(), args[1]);
  // Swap the engine callback before closing the previous sink, so a present
  // never posts to a released function.  If the swap throws, `sink` releases
  // its function on destruction.
  state.backend->SetRasterizedCallback(
      window_id, [sink, window_id]() { sink->Post(FrameEvent{window_id}); });
  auto existing = state.frame_sinks.find(window_id);
  if (existing != state.frame_sinks.end()) existing->second->Close();
  state.frame_sinks[window_id] = std::move(sink);
  return nullptr;
}

napi_value DispatchTouch(const Args& args) {
  AddonState& state = State(args.env());
  if (!state.backend) return nullptr;
  using Phase = skyline::WindowBackend::PointerPhase;
  const std::string kind = args.String(0);
  const double window_id = args.Number(1);
  const double x = args.Number(2);
  const double y = args.Number(3);
  Phase phase = Phase::kHover;
  if (kind == "start" || kind == "down") {
    phase = Phase::kDown;
  } else if (kind == "move") {
    phase = Phase::kMove;
  } else if (kind == "end" || kind == "up") {
    phase = Phase::kUp;
  } else if (kind == "cancel") {
    phase = Phase::kCancel;
  }
  if (skyline::config::Flag(skyline::config::kAppServiceDebug)) {
    AddonLog("dispatchTouch kind=%s window=%g x=%g y=%g", kind.c_str(),
             window_id, x, y);
  }
  state.backend->DispatchPointer(window_id, phase, x, y);
  return nullptr;
}

napi_value DispatchWheel(const Args& args) {
  AddonState& state = State(args.env());
  if (!state.backend) return nullptr;
  const double window_id = args.Number(0);
  const double x = args.Number(1);
  const double y = args.Number(2);
  const double delta_x = args.Number(3);
  const double delta_y = args.Number(4);
  if (skyline::config::Flag(skyline::config::kAppServiceDebug)) {
    AddonLog("dispatchWheel window=%g x=%g y=%g dx=%g dy=%g", window_id, x, y,
             delta_x, delta_y);
  }
  state.backend->DispatchWheel(window_id, x, y, delta_x, delta_y);
  return nullptr;
}

// -- Event sinks and callbacks ------------------------------------------------------

napi_value SetScrollSink(const Args& args) {
  if (!args.Is(0, napi_function)) {
    throw Error("setScrollSink expects a function");
  }
  ScrollEvents().Install(args.env(), args[0]);
  RUNTIME(SkylineSetScrollEventSink(&OnScrollEvent));
  return nullptr;
}

napi_value SetWidgetEventSink(const Args& args) {
  if (!args.Is(0, napi_function)) {
    throw Error("setWidgetEventSink expects a function");
  }
  WidgetEvents().Install(args.env(), args[0]);
  RUNTIME(SkylineSetWidgetEventSink(&OnWidgetEvent));
  return nullptr;
}

napi_value SetJsonEventSink(const Args& args) {
  if (!args.Is(0, napi_function)) {
    throw Error("setJsonEventSink expects a function");
  }
  JsonEvents().Install(args.env(), args[0]);
  RUNTIME(SkylineSetJsonEventSink(&OnJsonEvent));
  return nullptr;
}

// DevTools Network bridge.  Runtime image/font loads arrive on the engine UI
// thread; the pageframe JS callback registered through
// `SkylineShell.setHttpRequestCallback` performs the fetch (window.fetch) and
// calls `native.httpRequestComplete` with the bytes.
napi_value SetHttpRequestSink(const Args& args) {
  if (!args.Is(0, napi_function)) {
    throw Error("setHttpRequestSink expects a function");
  }
  HttpRequests().Install(args.env(), args[0]);
  RUNTIME(SkylineSetHttpRequestSink(&OnHttpRequest, &HasHttpRequestCallback));
  return nullptr;
}

// `httpRequestComplete(requestId, bytes)` from the pageframe fetch callback.
napi_value HttpRequestComplete(const Args& args) {
  void* data = nullptr;
  size_t length = 0;
  if (!args.Buffer(1, &data, &length)) {
    data = nullptr;
    length = 0;
  }
  RUNTIME(SkylineNotifyHttpRequestComplete(
      args.U32(0), static_cast<const uint8_t*>(data),
      static_cast<int32_t>(length), 0));
  return nullptr;
}

// SkylineShell's `set*Callback` setters.  The bootstrap keeps the callbacks;
// the addon only needs to know whether the pageframe can fetch for it.
napi_value SetCallback(const Args& args) {
  if (!args.Is(1, napi_function)) throw Error("Callback must be a function");
  if (args.String(0) == "setHttpRequestCallback") {
    g_http_request_callback_registered.store(true);
  }
  return nullptr;
}

// -- Page operations and queries ------------------------------------------------------

napi_value ApplyPageOps(const Args& args) {
  if (!args.Is(1, napi_string)) {
    throw Error("applyPageOps expects (windowId, json)");
  }
  const double window_id = args.Number(0);
  const std::string json = args.String(1);
  AddonLog("applyPageOps window=%g bytes=%zu", window_id, json.size());
  RUNTIME(SkylineApplyLiveOps(json.c_str(), window_id));
  return nullptr;
}

// Geometry for the appservice selector-query APIs.  The runtime keeps a
// mutex-protected snapshot published by the UI thread, so this synchronous
// call is safe from the JS thread.
napi_value GetNodeRect(const Args& args) {
  float rect[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  if (!RUNTIME_OR(SkylineGetNodeRect(args.U32(1), rect), 0)) return nullptr;
  napi_env env = args.env();
  napi_value object = NewObject(env);
  SetNumber(env, object, "left", rect[0]);
  SetNumber(env, object, "top", rect[1]);
  SetNumber(env, object, "right", rect[0] + rect[2]);
  SetNumber(env, object, "bottom", rect[1] + rect[3]);
  SetNumber(env, object, "width", rect[2]);
  SetNumber(env, object, "height", rect[3]);
  SetNumber(env, object, "zIndex", rect[4]);
  SetNumber(env, object, "pointerEvents", rect[5]);
  return object;
}

// scroll-into-view bridge: the appservice scroll-view component keeps the
// target id; the runtime scrolls the materialized Flutter widget.
napi_value ScrollNodeTo(const Args& args) {
  RUNTIME(SkylineScrollNodeTo(args.U32(1), args.Number(2)));
  return nullptr;
}

// The base library's scroll-view `scroll-into-view` property resolves the
// target element and calls `node.scrollIntoView(target, options)`.  Forward it
// to the owning native scroll view so horizontal/vertical scrolling and the
// center/start/end alignment policy are handled by the Dart widget.
napi_value ScrollIntoView(const Args& args) {
  RUNTIME(SkylineScrollIntoView(args.U32(1), args.U32(2),
                                args.Number(3) != 0.0 ? 1 : 0,
                                args.Number(4) != 0.0 ? 1 : 0, args.Number(5),
                                static_cast<uint8_t>(args.Number(6))));
  return nullptr;
}

// `swiperTo(windowId, nodeId, page[, animated])` — programmatic paging.
napi_value SwiperTo(const Args& args) {
  const double page = args.Number(2);
  RUNTIME(SkylineSwiperTo(args.U32(1),
                          static_cast<uint32_t>(page < 0 ? 0 : page),
                          args.Bool(3, true) ? 1 : 0));
  return nullptr;
}

// `pickerColumnSetCurrent(windowId, nodeId, current)`.
napi_value PickerColumnSetCurrent(const Args& args) {
  const double current = args.Number(2);
  RUNTIME(SkylinePickerColumnSetCurrent(
      args.U32(1), static_cast<uint32_t>(current < 0 ? 0 : current)));
  return nullptr;
}

// `inputFocus(windowId, nodeId, focus)` — programmatic focus/blur.
napi_value InputFocus(const Args& args) {
  RUNTIME(SkylineSetInputFocus(args.U32(1), args.Bool(2) ? 1 : 0));
  return nullptr;
}

// `addIntersectionObserver(windowId, nodeId, uniqueId, rootId, top, right,
// bottom, left[, thresholds])`.
napi_value AddIntersectionObserver(const Args& args) {
  napi_env env = args.env();
  std::vector<double> thresholds;
  napi_value list = args[8];
  bool is_array = false;
  if (list != nullptr && napi_is_array(env, list, &is_array) == napi_ok &&
      is_array) {
    uint32_t length = 0;
    napi_get_array_length(env, list, &length);
    for (uint32_t i = 0; i < length; i++) {
      napi_value item = nullptr;
      double value = 0.0;
      if (napi_get_element(env, list, i, &item) == napi_ok &&
          napi_get_value_double(env, item, &value) == napi_ok) {
        thresholds.push_back(value);
      }
    }
  }
  RUNTIME(SkylineAddIntersectionObserver(
      args.U32(1), args.U32(2), args.U32(3), args.Number(4), args.Number(5),
      args.Number(6), args.Number(7),
      thresholds.empty() ? nullptr : thresholds.data(),
      static_cast<int32_t>(thresholds.size())));
  return nullptr;
}

napi_value RemoveIntersectionObserver(const Args& args) {
  RUNTIME(SkylineRemoveIntersectionObserver(args.U32(1), args.U32(2)));
  return nullptr;
}

// `selectionSetRange(windowId, selectionId, startId, startOffset, endId,
// endOffset)`.
napi_value SelectionSetRange(const Args& args) {
  RUNTIME(SkylineSelectionSetRange(args.U32(1), args.U32(2), args.I32(3),
                                   args.U32(4), args.I32(5)));
  return nullptr;
}

// `refresherTriggered(windowId, nodeId, open[, twoLevel])`.
napi_value RefresherTriggered(const Args& args) {
  RUNTIME(SkylineRefresherTriggered(args.U32(1), args.Bool(2) ? 1 : 0,
                                    args.Bool(3) ? 1 : 0));
  return nullptr;
}

// `setNavigateBackInterception(windowId, pageId, enabled)`.
napi_value SetNavigateBackInterception(const Args& args) {
  RUNTIME(SkylineSetNavigateBackInterception(args.I32(1),
                                             args.Bool(2) ? 1 : 0));
  return nullptr;
}

// `setSafeArea(top, right, bottom, left)`; missing insets are 0.
napi_value SetSafeArea(const Args& args) {
  RUNTIME(SkylineSetSafeAreaInsets(args.Number(0), args.Number(1),
                                   args.Number(2), args.Number(3)));
  return nullptr;
}

// -- Accessibility and DevTools automation ----------------------------------------------

// D10: `setAccessibility(windowId, enabled)` toggles the appservice's
// accessibility mode programmatically (DevTools automation).
napi_value SetAccessibility(const Args& args) {
  RUNTIME(SkylineRequestAccessibility(args.Bool(1) ? 1 : 0));
  return nullptr;
}

napi_value AccessibilityState(const Args& args) {
  const auto state =
      static_cast<uint32_t>(RUNTIME_OR(SkylineGetAccessibilityState(), 0));
  napi_value result = nullptr;
  skyline::napi::Check(napi_create_uint32(args.env(), state, &result),
                       "napi_create_uint32");
  return result;
}

napi_value DumpElementTree(const Args&) {
  RUNTIME(SkylineDumpElementTree());
  return nullptr;
}

napi_value DumpRenderTree(const Args&) {
  RUNTIME(SkylineDumpRenderTree());
  return nullptr;
}

napi_value CollectGarbage(const Args&) {
  RUNTIME(SkylineCollectGarbage());
  return nullptr;
}

napi_value SendMessageToRunner(const Args& args) {
  RUNTIME(SkylineSendMessageToRunner(args.String(1).c_str()));
  return nullptr;
}

// `takeAppSnapshot(windowId, requestId[, scale[, compressed]])`.
napi_value TakeAppSnapshot(const Args& args) {
  RUNTIME(SkylineTakeAppSnapshot(args.U32(1), args.Number(2, 1.0),
                                 args.Bool(3, true) ? 1 : 0));
  return nullptr;
}

// `snapshotNode(windowId, nodeId, format, requestId)`.
napi_value SnapshotNode(const Args& args) {
  RUNTIME(SkylineSnapshotNode(args.U32(1),
                              static_cast<uint8_t>(args.Number(2, 4.0)),
                              args.U32(3)));
  return nullptr;
}

napi_value SetUserGesture(const Args& args) {
  RUNTIME(SkylineSetUserGesture(args.Bool(1) ? 1 : 0));
  return nullptr;
}

napi_value RelaunchApp(const Args& args) {
  RUNTIME(SkylineRelaunchApp(static_cast<uint8_t>(args.Number(1))));
  return nullptr;
}

napi_value ShutdownApplication(const Args&) {
  RUNTIME(SkylineShutdownApplication());
  return nullptr;
}

// -- Custom-route gesture worklets -----------------------------------------------------
//
// Custom route worklets run in the appservice realm (`workletUIModule`), so
// `didPop` / `startUserGesture` can only drive the Flutter route through these
// calls.

napi_value NavigatorPop(const Args& args) {
  RUNTIME(SkylineNavigatorPop(args.I32(1)));
  return nullptr;
}

napi_value StartUserGesture(const Args&) {
  RUNTIME(SkylineStartUserGesture());
  return nullptr;
}

napi_value StopUserGesture(const Args&) {
  RUNTIME(SkylineStopUserGesture());
  return nullptr;
}

napi_value UpdateAnimationController(const Args& args) {
  RUNTIME(SkylineUpdateAnimationController(args.I32(1), args.Number(2)));
  return nullptr;
}

// -- Misc ------------------------------------------------------------------------------

napi_value Noop(const Args&) { return nullptr; }

napi_value DebugInfo(const Args& args) {
  napi_env env = args.env();
  napi_value object = NewObject(env);
  skyline::napi::SetString(env, object, "skyline_git_rev",
                           "ef3edf6c94265dfe314f9197ff4e100f99aac070");
  skyline::napi::SetString(env, object, "flutter_engine_git_rev",
                           "6b917cf86ebe1b9ff148310504a736d667527e8b");
  skyline::napi::SetString(env, object, "skyline_version", "1.4.21");
  return object;
}

napi_value LogFromJs(const Args& args) {
  AddonLog("js: %s", args.String(0).c_str());
  return nullptr;
}

// The `native` object handed to the bootstrap: name, handler, minimum
// argument count and the error thrown below it (null: return undefined).
const Binding kBindings[] = {
    {"setCallback", SetCallback, 2, "Missing callback arguments"},
    {"createWindow", CreateWindow, 1, "createWindow expects an arguments array"},
    {"destroyWindow", DestroyWindow},
    {"applyPageOps", ApplyPageOps, 2, "applyPageOps expects (windowId, json)"},
    {"getNodeRect", GetNodeRect, 2},
    {"scrollNodeTo", ScrollNodeTo, 3},
    {"scrollIntoView", ScrollIntoView, 7},
    {"dispatchTouch", DispatchTouch, 4},
    {"dispatchWheel", DispatchWheel, 5},
    {"onWindowRasterized", OnWindowRasterized},
    {"setScrollSink", SetScrollSink},
    {"setWidgetEventSink", SetWidgetEventSink},
    {"swiperTo", SwiperTo, 3,
     "swiperTo expects (windowId, nodeId, page[, animated])"},
    {"pickerColumnSetCurrent", PickerColumnSetCurrent, 3,
     "pickerColumnSetCurrent expects (windowId, nodeId, current)"},
    {"inputFocus", InputFocus, 3,
     "inputFocus expects (windowId, nodeId, focus)"},
    {"setJsonEventSink", SetJsonEventSink},
    {"setHttpRequestSink", SetHttpRequestSink},
    {"httpRequestComplete", HttpRequestComplete, 2,
     "httpRequestComplete expects (requestId, bytes)"},
    {"addIntersectionObserver", AddIntersectionObserver, 8,
     "addIntersectionObserver expects 8 arguments"},
    {"removeIntersectionObserver", RemoveIntersectionObserver, 3,
     "removeIntersectionObserver expects (windowId, nodeId, id)"},
    {"selectionSetRange", SelectionSetRange, 6,
     "selectionSetRange expects 6 arguments"},
    {"refresherTriggered", RefresherTriggered, 3,
     "refresherTriggered expects (windowId, nodeId, open)"},
    {"setNavigateBackInterception", SetNavigateBackInterception, 3,
     "setNavigateBackInterception expects (windowId, pageId, enabled)"},
    {"setAccessibility", SetAccessibility, 2,
     "setAccessibility expects (windowId, enabled)"},
    {"accessibilityState", AccessibilityState},
    {"dumpElementTree", DumpElementTree},
    {"dumpRenderTree", DumpRenderTree},
    {"collectGarbage", CollectGarbage},
    {"sendMessageToRunner", SendMessageToRunner, 2,
     "sendMessageToRunner expects (windowId, message)"},
    {"takeAppSnapshot", TakeAppSnapshot, 2,
     "takeAppSnapshot expects (windowId, requestId[, scale[, compressed]])"},
    {"snapshotNode", SnapshotNode, 4,
     "snapshotNode expects (windowId, nodeId, format, requestId)"},
    {"setUserGesture", SetUserGesture, 2,
     "setUserGesture expects (windowId, active)"},
    {"navigatorPop", NavigatorPop, 2,
     "navigatorPop expects (windowId, pageId)"},
    {"startUserGesture", StartUserGesture},
    {"stopUserGesture", StopUserGesture},
    {"updateAnimationController", UpdateAnimationController, 3,
     "updateAnimationController expects (windowId, pageId, value)"},
    {"relaunchApp", RelaunchApp, 2, "relaunchApp expects (windowId, openType)"},
    {"shutdownApplication", ShutdownApplication},
    {"setSafeArea", SetSafeArea},
    {"dispatch", Noop},
    {"notify", Noop},
    {"brightness", Noop},
    {"debugInfo", DebugInfo},
    {"log", LogFromJs},
};

// Module lifecycle diagnostics.  The DevTools' appservice renderer has no
// accessible console, so a small append-only file makes it possible to tell a
// failed `require()` apart from a failure later in `createWindow`.
void AddonLog(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::fprintf(stderr, "[skyline-addon] ");
  std::vfprintf(stderr, format, args);
  std::fprintf(stderr, "\n");
  va_end(args);

  std::FILE* file = std::fopen("/tmp/skyline-addon.log", "a");
  if (file != nullptr) {
    std::fprintf(file, "[pid %d] ", static_cast<int>(::getpid()));
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fprintf(file, "\n");
    std::fclose(file);
  }
  std::fflush(stderr);
}

napi_value Init(napi_env env) {
  AddonLog("init begin");
  auto* state = new AddonState();
#ifdef SKYLINE_HAVE_ENGINE
  state->backend = skyline::CreateFlutterWindowBackend();
#else
  state->backend = std::make_unique<skyline::StubWindowBackend>();
#endif
  if (napi_set_instance_data(env, state, FinalizeState, nullptr) != napi_ok) {
    delete state;
    throw Error("napi_set_instance_data failed");
  }
  skyline::napi::Check(napi_add_env_cleanup_hook(env, CleanupCallbacks, state),
                       "napi_add_env_cleanup_hook");

  napi_value bridge = NewObject(env);
  skyline::napi::DefineBindings(env, bridge, kBindings,
                                sizeof(kBindings) / sizeof(kBindings[0]));
  napi_value global = nullptr;
  skyline::napi::Check(napi_get_global(env, &global), "napi_get_global");
  skyline::napi::Check(
      napi_set_named_property(env, global, "__skylineNative", bridge),
      "napi_set_named_property");

  // The bootstrap captures `__skylineNative` and installs SkylineShell and
  // friends on the global object.
  napi_value script = nullptr;
  skyline::napi::Check(napi_create_string_utf8(env, kSkylineBootstrapJs,
                                               NAPI_AUTO_LENGTH, &script),
                       "napi_create_string_utf8");
  napi_value ignored = nullptr;
  const napi_status status = napi_run_script(env, script, &ignored);

  napi_value key = nullptr;
  bool deleted = false;
  if (napi_create_string_utf8(env, "__skylineNative", NAPI_AUTO_LENGTH,
                              &key) == napi_ok) {
    napi_delete_property(env, global, key, &deleted);
  }

  if (status != napi_ok) {
    napi_value exception = nullptr;
    std::string message;
    if (napi_get_and_clear_last_exception(env, &exception) == napi_ok &&
        exception != nullptr) {
      message = skyline::napi::ToString(env, exception);
    }
    if (message.empty()) {
      message = "status " + std::to_string(static_cast<int>(status));
    }
    // Surface the cause to require() instead of an empty module.
    throw Error("skyline.node bootstrap failed: " + message);
  }
  AddonLog("init ok");

  // The official module has no named exports.
  return NewObject(env);
}

}  // namespace

NAPI_MODULE_INIT() {
  try {
    return Init(env);
  } catch (const std::exception& error) {
    AddonLog("init failed: %s", error.what());
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) {
      napi_throw_error(env, nullptr, error.what());
    }
    return nullptr;
  }
}

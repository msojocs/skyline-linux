#include <node_api.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "bootstrap_js.h"
#include "engine.hh"
#ifdef SKYLINE_HAVE_ENGINE
#include "host_engine.hh"
// Defined by the embedded Skyline runtime in libflutter_engine.so.
extern "C" void SkylineApplyLiveOps(const char* json, double window_id);
// Viewport-relative layout rect of a node (left, top, width, height).
extern "C" int32_t SkylineGetNodeRect(uint32_t node_id, float* out);
// Scroll a materialized scroll-view node to a vertical offset.
extern "C" void SkylineScrollNodeTo(uint32_t node_id, double top);
#endif

namespace {

void AddonLog(const char* format, ...);

napi_value Undefined(napi_env env) {
  napi_value value = nullptr;
  napi_get_undefined(env, &value);
  return value;
}

napi_value Throw(napi_env env, napi_status status, const char* message) {
  (void)status;
  napi_throw_error(env, nullptr, message);
  return nullptr;
}

std::string ReadString(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    return {};
  }
  std::string result(length, '\0');
  size_t copied = 0;
  napi_get_value_string_utf8(env, value, result.data(), length + 1, &copied);
  result.resize(copied);
  return result;
}

double ReadNumber(napi_env env, napi_value value, double fallback = 0.0) {
  double number = fallback;
  if (napi_get_value_double(env, value, &number) != napi_ok) return fallback;
  return number;
}

bool ReadBool(napi_env env, napi_value value, bool fallback = false) {
  bool result = fallback;
  if (napi_get_value_bool(env, value, &result) != napi_ok) return fallback;
  return result;
}

bool IsType(napi_env env, napi_value value, napi_valuetype expected) {
  napi_valuetype actual = napi_undefined;
  if (napi_typeof(env, value, &actual) != napi_ok) return false;
  return actual == expected;
}

struct AddonState {
  napi_env env = nullptr;
  std::map<std::string, napi_ref> callbacks;
  std::map<double, napi_threadsafe_function> frame_tsfns;
  std::unique_ptr<skyline::Engine> engine;
};

AddonState* State(napi_env env) {
  void* data = nullptr;
  if (napi_get_instance_data(env, &data) != napi_ok) return nullptr;
  return static_cast<AddonState*>(data);
}

void CleanupCallbacks(void* argument) {
  auto* state = static_cast<AddonState*>(argument);
  if (!state) return;
  // Stop engine threads before the environment and shared-memory views go
  // away; DestroyWindow joins each window's platform thread.
  state->engine.reset();
  for (auto& [window_id, tsfn] : state->frame_tsfns) {
    (void)window_id;
    if (tsfn) napi_release_threadsafe_function(tsfn, napi_tsfn_abort);
  }
  state->frame_tsfns.clear();
  for (auto& [name, reference] : state->callbacks) {
    (void)name;
    if (reference) napi_delete_reference(state->env, reference);
  }
  state->callbacks.clear();
}

void FinalizeState(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  auto* state = static_cast<AddonState*>(data);
  if (!state) return;
  // Window threads must be joined before the module state disappears.
  state->engine.reset();
  delete state;
}

napi_value SetCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return Throw(env, napi_invalid_arg, "Missing callback arguments");
  if (!IsType(env, argv[1], napi_function)) {
    return Throw(env, napi_invalid_arg, "Callback must be a function");
  }
  const std::string name = ReadString(env, argv[0]);
  auto* state = State(env);
  if (!state) return Throw(env, napi_generic_failure, "Addon state is unavailable");
  auto existing = state->callbacks.find(name);
  if (existing != state->callbacks.end()) {
    napi_delete_reference(env, existing->second);
    state->callbacks.erase(existing);
  }
  napi_ref reference = nullptr;
  if (napi_create_reference(env, argv[1], 1, &reference) != napi_ok) {
    return Throw(env, napi_generic_failure, "Unable to retain callback");
  }
  state->callbacks[name] = reference;
  return Undefined(env);
}

size_t TypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

// Returns the backing memory of an ArrayBuffer or typed-array view.  The
// pointer stays valid while the caller keeps the JS object alive.
bool ReadBufferView(napi_env env, napi_value value, void** data,
                    size_t* byte_length) {
  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) == napi_ok &&
      is_typed_array) {
    napi_typedarray_type type = napi_uint8_array;
    size_t length = 0;
    void* elements = nullptr;
    napi_value array_buffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env, value, &type, &length, &elements,
                                 &array_buffer, &byte_offset) != napi_ok ||
        elements == nullptr) {
      return false;
    }
    *data = elements;
    *byte_length = length * TypedArrayElementSize(type);
    return true;
  }
  bool is_array_buffer = false;
  if (napi_is_arraybuffer(env, value, &is_array_buffer) == napi_ok &&
      is_array_buffer) {
    void* buffer_data = nullptr;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, value, &buffer_data, &length) !=
            napi_ok ||
        buffer_data == nullptr) {
      return false;
    }
    *data = buffer_data;
    *byte_length = length;
    return true;
  }
  return false;
}

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
  napi_typedarray_type type = napi_uint8_array;
  size_t length = 0;
  void* elements = nullptr;
  napi_value array_buffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, value, &type, &length, &elements,
                               &array_buffer, &byte_offset) != napi_ok ||
      elements == nullptr || length * TypedArrayElementSize(type) != 16) {
    return false;
  }
  const auto* bytes = static_cast<const uint8_t*>(elements);
  uint64_t pointer = 0;
  uint64_t size = 0;
  for (int i = 0; i < 8; ++i) {
    pointer |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    size |= static_cast<uint64_t>(bytes[8 + i]) << (i * 8);
  }
  if (pointer == 0 || size < 8) return false;
  *data = reinterpret_cast<void*>(static_cast<uintptr_t>(pointer));
  *byte_length = static_cast<size_t>(size);
  return true;
}

skyline::CreateWindowArgs ParseCreateWindow(napi_env env, napi_value array) {
  skyline::CreateWindowArgs args;
  napi_value value = nullptr;
  if (napi_get_element(env, array, 0, &value) == napi_ok && IsType(env, value, napi_number)) {
    args.window_id = ReadNumber(env, value);
  }
  if (napi_get_element(env, array, 1, &value) == napi_ok && IsType(env, value, napi_string)) {
    args.bundle_path = ReadString(env, value);
  }
  if (napi_get_element(env, array, 2, &value) == napi_ok && IsType(env, value, napi_number)) {
    args.width = ReadNumber(env, value);
  }
  if (napi_get_element(env, array, 3, &value) == napi_ok && IsType(env, value, napi_number)) {
    args.height = ReadNumber(env, value);
  }
  if (napi_get_element(env, array, 4, &value) == napi_ok && IsType(env, value, napi_number)) {
    args.device_pixel_ratio = ReadNumber(env, value);
  }
  if (napi_get_element(env, array, 5, &value) == napi_ok && IsType(env, value, napi_boolean)) {
    args.hide_window = ReadBool(env, value);
  }
  if (napi_get_element(env, array, 7, &value) == napi_ok && IsType(env, value, napi_string)) {
    args.skyline_addon_path = ReadString(env, value);
  }
  // 6th argument: Linux shared-memory data area (ArrayBuffer / typed array).
  if (napi_get_element(env, array, 6, &value) == napi_ok && value != nullptr) {
    void* data = nullptr;
    size_t length = 0;
    if (ReadBufferView(env, value, &data, &length)) {
      args.frame_buffer = data;
      args.frame_buffer_size = length;
    }
  }
  // 9th argument: Windows 16-byte descriptor, or the live Linux ArrayBuffer
  // that the DevTools' sharedMemory.setMemory() returns.  A descriptor that
  // does not decode is ignored; a live view is only used when the 6th argument
  // did not already provide the frame buffer.
  if (napi_get_element(env, array, 8, &value) == napi_ok && value != nullptr) {
    void* data = nullptr;
    size_t length = 0;
    if (ReadSharedMemoryHandle(env, value, &data, &length)) {
      args.frame_buffer = data;
      args.frame_buffer_size = length;
    } else if (args.frame_buffer == nullptr &&
               ReadBufferView(env, value, &data, &length)) {
      args.frame_buffer = data;
      args.frame_buffer_size = length;
    }
  }
  return args;
}

napi_value CreateWindow(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || !IsType(env, argv[0], napi_object)) {
    return Throw(env, napi_invalid_arg, "createWindow expects an arguments array");
  }
  auto* state = State(env);
  if (!state || !state->engine) {
    return Throw(env, napi_generic_failure, "Skyline engine is unavailable");
  }
  try {
    const skyline::CreateWindowArgs args = ParseCreateWindow(env, argv[0]);
    AddonLog("createWindow id=%g bundle=%s addon=%s buffer=%p size=%zu",
             args.window_id, args.bundle_path.c_str(),
             args.skyline_addon_path.c_str(), args.frame_buffer,
             args.frame_buffer_size);
    state->engine->CreateWindow(args);
#ifdef SKYLINE_HAVE_ENGINE
    // DevTools integration demo: render a static page while the private
    // appservice JS bridge is unavailable.  The page ops use the same schema as
    // the recorded fixtures.
    if (const char* demo = std::getenv("SKYLINE_DEVTOOLS_DEMO_OPS")) {
      if (demo[0] != '\0') {
        std::ifstream file(demo);
        if (file) {
          const std::string json(
              (std::istreambuf_iterator<char>(file)),
              std::istreambuf_iterator<char>());
          if (!json.empty()) {
            SkylineApplyLiveOps(json.c_str(), args.window_id);
          }
        }
      }
    }
#endif
  } catch (const std::exception& error) {
    return Throw(env, napi_generic_failure, error.what());
  }
  return Undefined(env);
}

napi_value LogFromJs(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  const std::string message = argc >= 1 ? ReadString(env, argv[0]) : std::string();
  AddonLog("js: %s", message.c_str());
  return Undefined(env);
}

napi_value ApplyPageOps(napi_env env, napi_callback_info info) {  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2 || !IsType(env, argv[1], napi_string)) {
    return Throw(env, napi_invalid_arg, "applyPageOps expects (windowId, json)");
  }
  const double window_id = ReadNumber(env, argv[0]);
  const std::string json = ReadString(env, argv[1]);
  AddonLog("applyPageOps window=%g bytes=%zu", window_id, json.size());
#ifdef SKYLINE_HAVE_ENGINE
  SkylineApplyLiveOps(json.c_str(), window_id);
#else
  (void)window_id;
  (void)json;
#endif
  return Undefined(env);
}

// Geometry for the appservice selector-query APIs.  The runtime keeps a
// mutex-protected snapshot published by the UI thread, so this synchronous
// NAPI call is safe from the JS thread.
napi_value GetNodeRect(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) return Undefined(env);
  const double node_id = ReadNumber(env, argv[1]);
  float rect[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
#ifdef SKYLINE_HAVE_ENGINE
  if (!SkylineGetNodeRect(static_cast<uint32_t>(node_id), rect)) {
    return Undefined(env);
  }
#else
  (void)node_id;
  return Undefined(env);
#endif
  napi_value object = nullptr;
  napi_create_object(env, &object);
  const double values[8] = {rect[0], rect[1], rect[0] + rect[2],
                            rect[1] + rect[3], rect[2], rect[3], rect[4],
                            rect[5]};
  static const char* const keys[8] = {"left",  "top",   "right",
                                      "bottom", "width", "height",
                                      "zIndex", "pointerEvents"};
  for (int i = 0; i < 8; i++) {
    napi_value value = nullptr;
    napi_create_double(env, values[i], &value);
    napi_set_named_property(env, object, keys[i], value);
  }
  return object;
}

// scroll-into-view bridge: the appservice scroll-view component keeps the
// target id; the runtime scrolls the materialized Flutter widget.
napi_value ScrollNodeTo(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 3) return Undefined(env);
  const double node_id = ReadNumber(env, argv[1]);
  const double top = ReadNumber(env, argv[2]);
#ifdef SKYLINE_HAVE_ENGINE
  SkylineScrollNodeTo(static_cast<uint32_t>(node_id), top);
#else
  (void)node_id;
  (void)top;
#endif
  return Undefined(env);
}

napi_value DispatchTouch(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* state = State(env);
  if (!state || !state->engine || argc < 4) return Undefined(env);
  const std::string kind = ReadString(env, argv[0]);
  const double window_id = ReadNumber(env, argv[1]);
  const double x = ReadNumber(env, argv[2]);
  const double y = ReadNumber(env, argv[3]);
  skyline::Engine::PointerPhase phase = skyline::Engine::PointerPhase::kHover;
  if (kind == "start" || kind == "down") {
    phase = skyline::Engine::PointerPhase::kDown;
  } else if (kind == "move") {
    phase = skyline::Engine::PointerPhase::kMove;
  } else if (kind == "end" || kind == "up") {
    phase = skyline::Engine::PointerPhase::kUp;
  } else if (kind == "cancel") {
    phase = skyline::Engine::PointerPhase::kCancel;
  }
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    AddonLog("dispatchTouch kind=%s window=%g x=%g y=%g", kind.c_str(),
             window_id, x, y);
  }
  state->engine->DispatchPointer(window_id, phase, x, y);
  return Undefined(env);
}

napi_value DispatchWheel(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* state = State(env);
  if (!state || !state->engine || argc < 5) return Undefined(env);
  const double window_id = ReadNumber(env, argv[0]);
  const double x = ReadNumber(env, argv[1]);
  const double y = ReadNumber(env, argv[2]);
  const double delta_x = ReadNumber(env, argv[3]);
  const double delta_y = ReadNumber(env, argv[4]);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    AddonLog("dispatchWheel window=%g x=%g y=%g dx=%g dy=%g", window_id, x, y,
             delta_x, delta_y);
  }
  state->engine->DispatchWheel(window_id, x, y, delta_x, delta_y);
  return Undefined(env);
}

napi_value DestroyWindow(napi_env env, napi_callback_info info) {  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* state = State(env);
  if (state && state->engine) {
    const double window_id = argc > 0 ? ReadNumber(env, argv[0]) : 0.0;
    state->engine->DestroyWindow(window_id);
    auto tsfn = state->frame_tsfns.find(window_id);
    if (tsfn != state->frame_tsfns.end()) {
      if (tsfn->second) {
        napi_release_threadsafe_function(tsfn->second, napi_tsfn_release);
      }
      state->frame_tsfns.erase(tsfn);
    }
  }
  return Undefined(env);
}

struct FrameCallbackContext {
  double window_id = 0.0;
};

void FinalizeFrameCallbackContext(napi_env /*env*/, void* data,
                                  void* /*hint*/) {
  delete static_cast<FrameCallbackContext*>(data);
}

void CallFrameCallback(napi_env env, napi_value js_callback, void* context,
                       void* /*data*/) {
  if (env == nullptr || js_callback == nullptr) return;
  auto* callback_context = static_cast<FrameCallbackContext*>(context);
  // The DevTools waits for `{window_id}` to forward
  // SKYLINE_WINDOW_RASTERIZED to the pageframe; a bare callback invocation
  // makes `e.window_id` throw and the pageframe never pulls the new frame.
  napi_value event = nullptr;
  napi_create_object(env, &event);
  napi_value window_id = nullptr;
  napi_create_double(
      env, callback_context != nullptr ? callback_context->window_id : 0.0,
      &window_id);
  napi_set_named_property(env, event, "window_id", window_id);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  napi_value result = nullptr;
  napi_call_function(env, undefined, js_callback, 1, &event, &result);
}

napi_value OnWindowRasterized(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* state = State(env);
  if (!state || !state->engine) {
    return Throw(env, napi_generic_failure, "Skyline engine is unavailable");
  }
  const double window_id = ReadNumber(env, argv[0]);
  if (!state->engine->HasWindow(window_id)) {
    return Throw(env, napi_generic_failure,
                 ("window_id: " + std::to_string(
                      static_cast<long long>(window_id)) + " not found")
                     .c_str());
  }
  if (argc < 2 || !IsType(env, argv[1], napi_function)) {
    return Throw(env, napi_invalid_arg, "Callback must be a function");
  }
  try {
    napi_threadsafe_function tsfn = nullptr;
    napi_value resource_name = nullptr;
    napi_create_string_utf8(env, "skyline_frame", NAPI_AUTO_LENGTH,
                            &resource_name);
    // max_queue_size=1: frame callbacks are wake-ups, not data; coalesce
    // bursts while the Node event loop is busy.
    auto* callback_context = new FrameCallbackContext{window_id};
    if (napi_create_threadsafe_function(
            env, argv[1], nullptr, resource_name, 1, 1, callback_context,
            &FinalizeFrameCallbackContext, callback_context,
            &CallFrameCallback, &tsfn) != napi_ok) {
      delete callback_context;
      return Throw(env, napi_generic_failure,
                   "Unable to create frame callback");
    }
    auto existing = state->frame_tsfns.find(window_id);
    if (existing != state->frame_tsfns.end()) {
      if (existing->second) {
        napi_release_threadsafe_function(existing->second, napi_tsfn_release);
      }
      state->frame_tsfns.erase(existing);
    }
    state->frame_tsfns[window_id] = tsfn;
    state->engine->SetRasterizedCallback(
        window_id, [tsfn]() {
          napi_call_threadsafe_function(tsfn, nullptr, napi_tsfn_nonblocking);
        });
  } catch (const std::exception& error) {
    return Throw(env, napi_generic_failure, error.what());
  }
  return Undefined(env);
}

napi_value Noop(napi_env env, napi_callback_info info) {
  (void)info;
  return Undefined(env);
}

napi_value DebugInfo(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value object = nullptr;
  napi_create_object(env, &object);
  struct Entry { const char* key; const char* value; };
  const Entry entries[] = {
      {"skyline_git_rev", "ef3edf6c94265dfe314f9197ff4e100f99aac070"},
      {"flutter_engine_git_rev", "6b917cf86ebe1b9ff148310504a736d667527e8b"},
      {"skyline_version", "1.4.21"},
  };
  for (const auto& entry : entries) {
    napi_value value = nullptr;
    napi_create_string_utf8(env, entry.value, NAPI_AUTO_LENGTH, &value);
    napi_set_named_property(env, object, entry.key, value);
  }
  return object;
}

void Define(napi_env env, napi_value target, const char* name, napi_callback callback) {
  napi_value function = nullptr;
  napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, nullptr, &function);
  napi_set_named_property(env, target, name, function);
}

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

std::string NapiValueToString(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok ||
      string_value == nullptr) {
    return {};
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) !=
      napi_ok) {
    return {};
  }
  std::vector<char> buffer(length + 1, '\0');
  size_t copied = 0;
  napi_get_value_string_utf8(env, string_value, buffer.data(), buffer.size(),
                             &copied);
  return std::string(buffer.data(), copied);
}

}  // namespace

NAPI_MODULE_INIT() {
  AddonLog("init begin");
  auto* state = new AddonState();
  state->env = env;
#ifdef SKYLINE_HAVE_ENGINE
  state->engine = skyline::CreateHostEngine();
#else
  state->engine = std::make_unique<skyline::StubEngine>();
#endif
  napi_set_instance_data(env, state, FinalizeState, nullptr);
  napi_add_env_cleanup_hook(env, CleanupCallbacks, state);

  napi_value bridge = nullptr;
  napi_create_object(env, &bridge);
  Define(env, bridge, "setCallback", SetCallback);
  Define(env, bridge, "createWindow", CreateWindow);
  Define(env, bridge, "destroyWindow", DestroyWindow);
  Define(env, bridge, "applyPageOps", ApplyPageOps);
  Define(env, bridge, "getNodeRect", GetNodeRect);
  Define(env, bridge, "scrollNodeTo", ScrollNodeTo);
  Define(env, bridge, "dispatchTouch", DispatchTouch);
  Define(env, bridge, "dispatchWheel", DispatchWheel);
  Define(env, bridge, "onWindowRasterized", OnWindowRasterized);
  Define(env, bridge, "setSafeArea", Noop);
  Define(env, bridge, "dispatch", Noop);
  Define(env, bridge, "notify", Noop);
  Define(env, bridge, "brightness", Noop);
  Define(env, bridge, "debugInfo", DebugInfo);
  Define(env, bridge, "log", LogFromJs);

  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_set_named_property(env, global, "__skylineNative", bridge);

  napi_value script = nullptr;
  napi_create_string_utf8(env, kSkylineBootstrapJs, NAPI_AUTO_LENGTH, &script);
  napi_value ignored = nullptr;
  const napi_status status = napi_run_script(env, script, &ignored);

  napi_value key = nullptr;
  napi_create_string_utf8(env, "__skylineNative", NAPI_AUTO_LENGTH, &key);
  bool deleted = false;
  napi_delete_property(env, global, key, &deleted);

  if (status != napi_ok) {
    napi_value exception = nullptr;
    const napi_status exception_status =
        napi_get_and_clear_last_exception(env, &exception);
    std::string message;
    if (exception_status == napi_ok && exception != nullptr) {
      message = NapiValueToString(env, exception);
    }
    AddonLog("bootstrap failed: status=%d exception=%s", static_cast<int>(status),
             message.empty() ? "(none)" : message.c_str());
    return nullptr;
  }
  AddonLog("init ok");

  // The official module has no named exports.
  napi_value exports_object = nullptr;
  napi_create_object(env, &exports_object);
  return exports_object;
}

#include <node_api.h>

#include <map>
#include <memory>
#include <string>

#include "bootstrap_js.h"
#include "engine.hh"

namespace {

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
  for (auto& [name, reference] : state->callbacks) {
    (void)name;
    if (reference) napi_delete_reference(state->env, reference);
  }
  state->callbacks.clear();
}

void FinalizeState(napi_env env, void* data, void* hint) {
  (void)env;
  (void)hint;
  delete static_cast<AddonState*>(data);
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
    state->engine->CreateWindow(ParseCreateWindow(env, argv[0]));
  } catch (const std::exception& error) {
    return Throw(env, napi_generic_failure, error.what());
  }
  return Undefined(env);
}

napi_value DestroyWindow(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* state = State(env);
  if (state && state->engine) {
    const double window_id = argc > 0 ? ReadNumber(env, argv[0]) : 0.0;
    state->engine->DestroyWindow(window_id);
  }
  return Undefined(env);
}

napi_value OnWindowRasterized(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  auto* state = State(env);
  if (!state || !state->engine) {
    return Throw(env, napi_generic_failure, "Skyline engine is unavailable");
  }
  try {
    state->engine->SetWindowRasterized(ReadNumber(env, argv[0]));
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

}  // namespace

NAPI_MODULE_INIT() {
  auto* state = new AddonState();
  state->env = env;
  state->engine = std::make_unique<skyline::StubEngine>();
  napi_set_instance_data(env, state, FinalizeState, nullptr);
  napi_add_env_cleanup_hook(env, CleanupCallbacks, state);

  napi_value bridge = nullptr;
  napi_create_object(env, &bridge);
  Define(env, bridge, "setCallback", SetCallback);
  Define(env, bridge, "createWindow", CreateWindow);
  Define(env, bridge, "destroyWindow", DestroyWindow);
  Define(env, bridge, "onWindowRasterized", OnWindowRasterized);
  Define(env, bridge, "setSafeArea", Noop);
  Define(env, bridge, "dispatch", Noop);
  Define(env, bridge, "notify", Noop);
  Define(env, bridge, "brightness", Noop);
  Define(env, bridge, "debugInfo", DebugInfo);

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

  if (status != napi_ok) return nullptr;

  // The official module has no named exports.
  napi_value exports_object = nullptr;
  napi_create_object(env, &exports_object);
  return exports_object;
}

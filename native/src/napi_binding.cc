#include "napi_binding.hh"

#include <vector>

namespace skyline::napi {
namespace {

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

// The one napi_callback behind every binding; `data` is the Binding.
napi_value Dispatch(napi_env env, napi_callback_info info) {
  try {
    void* data = nullptr;
    const Args args(env, info, &data);
    const auto* binding = static_cast<const Binding*>(data);
    if (args.size() < binding->min_args) {
      if (binding->usage != nullptr) throw Error(binding->usage);
      return Undefined(env);
    }
    napi_value result = binding->handler(args);
    return result != nullptr ? result : Undefined(env);
  } catch (const std::exception& error) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) {
      napi_throw_error(env, nullptr, error.what());
    }
    return nullptr;
  }
}

}  // namespace

void Check(napi_status status, const char* what) {
  if (status != napi_ok) throw Error(std::string(what) + " failed");
}

Args::Args(napi_env env, napi_callback_info info, void** data) : env_(env) {
  size_t count = kCapacity;
  Check(napi_get_cb_info(env, info, &count, values_, nullptr, data),
        "napi_get_cb_info");
  count_ = count;
}

bool Args::Is(size_t index, napi_valuetype type) const {
  napi_value value = (*this)[index];
  napi_valuetype actual = napi_undefined;
  if (value == nullptr || napi_typeof(env_, value, &actual) != napi_ok) {
    return false;
  }
  return actual == type;
}

double Args::Number(size_t index, double fallback) const {
  napi_value value = (*this)[index];
  double number = fallback;
  if (value == nullptr ||
      napi_get_value_double(env_, value, &number) != napi_ok) {
    return fallback;
  }
  return number;
}

bool Args::Bool(size_t index, bool fallback) const {
  napi_value value = (*this)[index];
  bool result = fallback;
  if (value == nullptr || napi_get_value_bool(env_, value, &result) != napi_ok) {
    return fallback;
  }
  return result;
}

std::string Args::String(size_t index) const {
  napi_value value = (*this)[index];
  size_t length = 0;
  if (value == nullptr ||
      napi_get_value_string_utf8(env_, value, nullptr, 0, &length) !=
          napi_ok) {
    return {};
  }
  std::string result(length, '\0');
  size_t copied = 0;
  napi_get_value_string_utf8(env_, value, result.data(), length + 1, &copied);
  result.resize(copied);
  return result;
}

bool Args::Buffer(size_t index, void** data, size_t* byte_length) const {
  napi_value value = (*this)[index];
  return value != nullptr && ReadBufferView(env_, value, data, byte_length);
}

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

napi_value Undefined(napi_env env) {
  napi_value value = nullptr;
  Check(napi_get_undefined(env, &value), "napi_get_undefined");
  return value;
}

napi_value NewObject(napi_env env) {
  napi_value object = nullptr;
  Check(napi_create_object(env, &object), "napi_create_object");
  return object;
}

void SetNumber(napi_env env, napi_value object, const char* key,
               double value) {
  napi_value number = nullptr;
  Check(napi_create_double(env, value, &number), "napi_create_double");
  Check(napi_set_named_property(env, object, key, number),
        "napi_set_named_property");
}

void SetBool(napi_env env, napi_value object, const char* key, bool value) {
  napi_value boolean = nullptr;
  Check(napi_get_boolean(env, value, &boolean), "napi_get_boolean");
  Check(napi_set_named_property(env, object, key, boolean),
        "napi_set_named_property");
}

void SetString(napi_env env, napi_value object, const char* key,
               const char* value) {
  napi_value string = nullptr;
  Check(napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &string),
        "napi_create_string_utf8");
  Check(napi_set_named_property(env, object, key, string),
        "napi_set_named_property");
}

std::string ToString(napi_env env, napi_value value) {
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

void DefineBindings(napi_env env, napi_value target, const Binding* bindings,
                    size_t count) {
  for (size_t i = 0; i < count; ++i) {
    napi_value function = nullptr;
    Check(napi_create_function(env, bindings[i].name, NAPI_AUTO_LENGTH,
                               &Dispatch,
                               const_cast<Binding*>(&bindings[i]), &function),
          "napi_create_function");
    Check(napi_set_named_property(env, target, bindings[i].name, function),
          "napi_set_named_property");
  }
}

}  // namespace skyline::napi

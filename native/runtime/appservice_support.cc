// Shared replay support: isolate/scope, UI task runner, resource paths.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_support.hh"

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
fml::RefPtr<fml::TaskRunner> UiTaskRunner() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  auto runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
#pragma GCC diagnostic pop
  return runner;
}
std::string ResolveResourcePath(const std::string& path) {
  std::string resolved = path;
  if (resolved.rfind("file://", 0) == 0) {
    resolved = resolved.substr(7);
  }
  if (resolved.empty() || resolved[0] == '/') return resolved;
  std::string base = config::String(config::kResourceRoot);
  if (base.empty()) return resolved;
  if (base.back() != '/') base.push_back('/');
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

namespace appservice {

namespace {
float g_safe_area_insets[4] = {0.0f, 0.0f, 0.0f, 0.0f};
}  // namespace

// Safe-area insets pushed by `SkylineShell.setSafeAreaEdgeInsets`.
void SetSafeAreaInsets(double top, double right, double bottom, double left) {
  g_safe_area_insets[0] = static_cast<float>(top);
  g_safe_area_insets[1] = static_cast<float>(right);
  g_safe_area_insets[2] = static_cast<float>(bottom);
  g_safe_area_insets[3] = static_cast<float>(left);
}

float SafeAreaInset(int side) {
  if (side < 0 || side > 3) return 0.0f;
  return g_safe_area_insets[side];
}

}  // namespace appservice

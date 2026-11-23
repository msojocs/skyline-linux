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

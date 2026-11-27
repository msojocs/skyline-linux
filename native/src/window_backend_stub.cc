#include "window_backend.hh"

#include <string>

namespace skyline {

void StubWindowBackend::CreateWindow(const CreateWindowArgs& args) {
  (void)args;
  throw std::runtime_error(
      "Skyline engine backend is not attached (native stub)");
}

void StubWindowBackend::DestroyWindow(double window_id) {
  // No window can exist without an engine; mirror the native module's benign
  // no-op instead of inventing an error path.
  (void)window_id;
}

bool StubWindowBackend::HasWindow(double window_id) const {
  (void)window_id;
  return false;
}

void StubWindowBackend::SetRasterizedCallback(double window_id,
                                       RasterizedCallback callback) {
  (void)callback;
  throw std::runtime_error("window_id: " + std::to_string(
      static_cast<long long>(window_id)) + " not found");
}

}  // namespace skyline

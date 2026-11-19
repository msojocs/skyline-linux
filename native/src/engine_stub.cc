#include "engine.hh"

#include <string>

namespace skyline {

void StubEngine::CreateWindow(const CreateWindowArgs& args) {
  (void)args;
  throw std::runtime_error(
      "Skyline engine backend is not attached (native stub)");
}

void StubEngine::DestroyWindow(double window_id) {
  // No window can exist without an engine; mirror the native module's benign
  // no-op instead of inventing an error path.
  (void)window_id;
}

void StubEngine::SetWindowRasterized(double window_id) {
  throw std::runtime_error("window_id: " + std::to_string(
      static_cast<long long>(window_id)) + " not found");
}

}  // namespace skyline

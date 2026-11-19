#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace skyline {

struct CreateWindowArgs {
  double window_id = 0;
  std::string bundle_path;
  double width = 0;
  double height = 0;
  double device_pixel_ratio = 0;
  bool hide_window = false;
  std::string skyline_addon_path;
};

// Backend boundary for the native API layer.  The stub implementation reports
// that no engine is attached; the Linux Flutter Engine backend replaces it in
// phase 2 without changing the JS-visible surface.
class Engine {
 public:
  virtual ~Engine() = default;
  virtual void CreateWindow(const CreateWindowArgs& args) = 0;
  virtual void DestroyWindow(double window_id) = 0;
  virtual void SetWindowRasterized(double window_id) = 0;
};

class StubEngine final : public Engine {
 public:
  void CreateWindow(const CreateWindowArgs& args) override;
  void DestroyWindow(double window_id) override;
  void SetWindowRasterized(double window_id) override;
};

}  // namespace skyline

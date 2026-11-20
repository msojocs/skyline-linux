#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
  // Frame destination supplied by the caller: either a typed-array view (6th
  // createWindow argument, Linux shared-memory data area) or a raw pointer from
  // the 16-byte Windows shared-memory handle (9th argument).
  void* frame_buffer = nullptr;
  size_t frame_buffer_size = 0;
};

// Backend boundary for the native API layer.  The stub implementation reports
// that no engine is attached; the Linux Flutter Engine backend replaces it in
// phase 2 without changing the JS-visible surface.
class Engine {
 public:
  // Invoked once per presented frame; called from the engine's raster thread.
  using RasterizedCallback = std::function<void()>;

  virtual ~Engine() = default;
  virtual void CreateWindow(const CreateWindowArgs& args) = 0;
  virtual void DestroyWindow(double window_id) = 0;
  // Returns false for unknown window ids; used for `onWindowRasterized`
  // validation before the callback is registered.
  virtual bool HasWindow(double window_id) const = 0;
  virtual void SetRasterizedCallback(double window_id,
                                     RasterizedCallback callback) = 0;
};

class StubEngine final : public Engine {
 public:
  void CreateWindow(const CreateWindowArgs& args) override;
  void DestroyWindow(double window_id) override;
  bool HasWindow(double window_id) const override;
  void SetRasterizedCallback(double window_id,
                             RasterizedCallback callback) override;
};

}  // namespace skyline

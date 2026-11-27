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

// What skyline.node needs from the engine host: window lifecycle, simulator
// input and presented frames.  Page operations, queries, scrolling, sinks and
// automation go straight to the embedded runtime (skyline/runtime_api.h).
class WindowBackend {
 public:
  enum class PointerPhase : uint8_t {
    kDown = 0,
    kMove = 1,
    kUp = 2,
    kCancel = 3,
    kHover = 4,
  };

  // Invoked once per presented frame; called from the engine's raster thread.
  using RasterizedCallback = std::function<void()>;

  virtual ~WindowBackend() = default;
  virtual void CreateWindow(const CreateWindowArgs& args) = 0;
  virtual void DestroyWindow(double window_id) = 0;
  // Returns false for unknown window ids; used for `onWindowRasterized`
  // validation before the callback is registered.
  virtual bool HasWindow(double window_id) const = 0;
  // Replaces the window's callback.  A present that started before the call
  // may still invoke the previous callback once.
  virtual void SetRasterizedCallback(double window_id,
                                     RasterizedCallback callback) = 0;
  // DevTools simulator input.  Coordinates are logical (CSS) pixels; the host
  // scales them by the window's device pixel ratio before forwarding to
  // Flutter.
  virtual void DispatchPointer(double window_id, PointerPhase phase, double x,
                               double y) {
    (void)window_id;
    (void)phase;
    (void)x;
    (void)y;
  }
  virtual void DispatchWheel(double window_id, double x, double y,
                             double delta_x, double delta_y) {
    (void)window_id;
    (void)x;
    (void)y;
    (void)delta_x;
    (void)delta_y;
  }
};

// Used when skyline.node is built without an engine: every window operation
// reports that no backend is attached.
class StubWindowBackend final : public WindowBackend {
 public:
  void CreateWindow(const CreateWindowArgs& args) override;
  void DestroyWindow(double window_id) override;
  bool HasWindow(double window_id) const override;
  void SetRasterizedCallback(double window_id,
                             RasterizedCallback callback) override;
};

}  // namespace skyline

// Linux Flutter Engine backend for `skyline.node` (phase 2).
//
// The embedder API is not thread-safe across platform threads: the thread that
// calls `FlutterEngineRun` becomes the platform thread and must also service
// vsync batons and shut the engine down.  Each `CreateWindow` therefore starts
// its own platform thread and keeps it for the window's lifetime.  Presented
// frames are copied (BGRA -> RGBA) into the caller-supplied shared-memory data
// area and the Node callback is dispatched through a threadsafe function.

#include "flutter_window_backend.hh"

#include <flutter_embedder.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "embedder/embedder.hh"
#include "skyline/config.h"
#include "skyline/frame_protocol.h"
#include "skyline/log.h"
#include "skyline/runtime_api.h"

namespace skyline {
namespace {

namespace config = skyline::config;
using embedder::FileExists;
using embedder::JoinPath;

constexpr char kLog[] = "skyline-engine";
constexpr int32_t kTouchDevice = 1;
constexpr int32_t kMouseDevice = 2;

std::string ResolveAssets(const std::string& bundle_path) {
  const std::string& override_path = config::String(config::kAssets);
  if (!override_path.empty()) return override_path;
  const std::string nested = JoinPath(bundle_path, "flutter_assets");
  if (FileExists(JoinPath(nested, "kernel_blob.bin"))) return nested;
  if (FileExists(JoinPath(bundle_path, "kernel_blob.bin"))) return bundle_path;
  return nested;
}

std::string ResolveAot(const std::string& bundle_path,
                       const std::string& assets) {
  const std::string& override_path = config::String(config::kAotLibrary);
  if (!override_path.empty() && FileExists(override_path)) {
    return override_path;
  }
  return embedder::FindAotLibrary(bundle_path, assets);
}

std::string ResolveIcu(const std::string& bundle_path) {
  for (const std::string& candidate :
       {JoinPath(bundle_path, "icudtl.dat"),
        JoinPath(JoinPath(bundle_path, "flutter_assets"), "icudtl.dat")}) {
    if (FileExists(candidate)) return candidate;
  }
  const std::string& override_path = config::String(config::kIcu);
  if (!override_path.empty() && FileExists(override_path)) {
    return override_path;
  }
#ifdef SKYLINE_DEFAULT_ICU
  if (FileExists(SKYLINE_DEFAULT_ICU)) return SKYLINE_DEFAULT_ICU;
#endif
  return {};
}

void LogMessage(const char* tag, const char* message, void* /*user_data*/) {
  embedder::LogEngineMessage(kLog, tag, message);
}

void OnSemanticsUpdate(const FlutterSemanticsUpdate2* update, void* /*data*/) {
  embedder::LogSemanticsUpdate(kLog, update);
}

class WindowHost {
 public:
  explicit WindowHost(CreateWindowArgs args)
      : args_(std::move(args)),
        loop_(kLog, "window " + FormatId(args_.window_id) + " ") {}
  ~WindowHost() { Stop(); }

  WindowHost(const WindowHost&) = delete;
  WindowHost& operator=(const WindowHost&) = delete;

  void Start() {
    const double width = args_.width;
    const double height = args_.height;
    const double dpr = args_.device_pixel_ratio;
    if (!(width > 0.0) || !(height > 0.0) || !(dpr > 0.0) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(dpr)) {
      throw std::runtime_error("invalid window dimensions or pixel ratio");
    }
    assets_ = ResolveAssets(args_.bundle_path);
    const bool has_kernel = FileExists(JoinPath(assets_, "kernel_blob.bin"));
    use_aot_ = !has_kernel || !config::String(config::kAotLibrary).empty();
    if (use_aot_) {
      aot_library_ = ResolveAot(args_.bundle_path, assets_);
    }
    icu_ = ResolveIcu(args_.bundle_path);
    if (!has_kernel && aot_library_.empty()) {
      throw std::runtime_error(
          "missing kernel_blob.bin and libapp.so under " + assets_);
    }
    if (use_aot_ && aot_library_.empty()) {
      throw std::runtime_error("AOT snapshot requested but libapp.so is missing");
    }
    if (icu_.empty()) {
      throw std::runtime_error(
          "icudtl.dat not found (set SKYLINE_ICU or install the engine stage)");
    }
    pixel_ratio_ = dpr;

    thread_ = std::thread([this]() { Run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_cv_.wait_for(lock, std::chrono::seconds(15),
                            [this]() { return ready_; })) {
      lock.unlock();
      loop_.Stop();
      if (thread_.joinable()) thread_.join();
      throw std::runtime_error("Flutter engine did not start in time");
    }
    const std::string error = init_error_;
    lock.unlock();
    if (!error.empty()) {
      if (thread_.joinable()) thread_.join();
      throw std::runtime_error(error);
    }
  }

  void Stop() {
    loop_.Stop();
    if (thread_.joinable()) thread_.join();
  }

  void SetRasterizedCallback(WindowBackend::RasterizedCallback callback) {
    WindowBackend::RasterizedCallback notify_now;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback_ = std::move(callback);
      // The first frame may be presented before `onWindowRasterized` is
      // registered (createWindow -> onWindowRasterized is two calls).
      if (frame_id_ > 0 && callback_) notify_now = callback_;
    }
    if (notify_now) notify_now();
  }

  void SetFrameBuffer(void* buffer, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_buffer_ = buffer;
    frame_buffer_size_ = size;
  }

  // DevTools simulator input.  The caller passes logical (CSS) pixels; Flutter
  // expects physical pixels for the implicit view.
  //
  // `FlutterEngineSendPointerEvent` is platform-thread-only (the window's
  // `Run` thread).  The NAPI/JS thread must not call it directly: doing so
  // deadlocks the appservice webview on touch and trips the engine's
  // weak_ptr thread check.  Marshal the event onto the platform thread and
  // keep the mailbox FIFO so down/move/up stay ordered.
  void SendPointer(WindowBackend::PointerPhase phase, double x, double y) {
    FlutterPointerEvent event = {};
    event.struct_size = sizeof(event);
    event.timestamp = FlutterEngineGetCurrentTime();
    event.x = x * pixel_ratio_;
    event.y = y * pixel_ratio_;
    // Touch and mouse need distinct device ids: the engine keeps one pointer
    // state per device, so hovering with a "touch" device while it is down
    // trips the converter's !is_down checks.
    event.device = phase == WindowBackend::PointerPhase::kHover ? kMouseDevice
                                                               : kTouchDevice;
    event.view_id = 0;
    event.signal_kind = kFlutterPointerSignalKindNone;
    switch (phase) {
      case WindowBackend::PointerPhase::kDown:
        event.phase = kDown;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case WindowBackend::PointerPhase::kMove:
        event.phase = kMove;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case WindowBackend::PointerPhase::kUp:
        event.phase = kUp;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case WindowBackend::PointerPhase::kCancel:
        event.phase = kCancel;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case WindowBackend::PointerPhase::kHover:
        event.phase = kHover;
        event.device_kind = kFlutterPointerDeviceKindMouse;
        break;
    }
    PostPlatformTask([this, event, phase]() {
      // Synthetic touch sequences from the simulator can repeat a phase (for
      // example a click handler firing after a touch sequence).  The engine
      // aborts the whole process on a duplicate down / orphan move / up, so
      // keep the single-touch state machine coherent here.
      if (event.device_kind == kFlutterPointerDeviceKindTouch) {
        if (phase == WindowBackend::PointerPhase::kDown) {
          if (touch_down_) return;
          touch_down_ = true;
        } else if (phase == WindowBackend::PointerPhase::kUp ||
                   phase == WindowBackend::PointerPhase::kCancel) {
          if (!touch_down_) return;
          touch_down_ = false;
        } else if (phase == WindowBackend::PointerPhase::kMove) {
          if (!touch_down_) return;
        }
      }
      FlutterEngineSendPointerEvent(engine_->get(), &event, 1);
    });
  }

  void SendWheel(double x, double y, double delta_x, double delta_y) {
    FlutterPointerEvent event = {};
    event.struct_size = sizeof(event);
    event.phase = kMove;
    event.timestamp = FlutterEngineGetCurrentTime();
    event.x = x * pixel_ratio_;
    event.y = y * pixel_ratio_;
    event.device = kMouseDevice;
    event.view_id = 0;
    event.device_kind = kFlutterPointerDeviceKindMouse;
    event.signal_kind = kFlutterPointerSignalKindScroll;
    event.scroll_delta_x = delta_x;
    event.scroll_delta_y = delta_y;
    PostPlatformTask([this, event]() {
      FlutterEngineSendPointerEvent(engine_->get(), &event, 1);
    });
  }

 private:
  static std::string FormatId(double window_id) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", window_id);
    return text;
  }

  // Queues a closure for the platform thread (the `Run` thread owns the
  // engine and is the only thread allowed to call the input embedder APIs).
  void PostPlatformTask(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!ready_ || !init_error_.empty()) return;
    }
    loop_.Post(std::move(task));
  }

  void Run() {
    // The DevTools' Skyline Dart entrypoint (`package:sample/devtools.dart
    // main`) expects [window id, runtime holder, width, height, createRuntime,
    // addon path].  Plain test apps use a zero-argument main, so only pass
    // these for the DevTools packaging.  The DevTools always loads the addon
    // from its own `node_modules/skyline-addon/` directory, which is enough to
    // recognize it without requiring environment variables.
    embedder::EngineConfig config;
    config.assets = assets_;
    config.icu = icu_;
    if (use_aot_) config.aot_library = aot_library_;
    const bool devtools_entrypoint =
        config::Flag(config::kDevtools) ||
        args_.bundle_path.find("skyline-addon") != std::string::npos ||
        args_.skyline_addon_path.find("skyline-addon") != std::string::npos;
    if (devtools_entrypoint) {
      config.entrypoint_args = {
          std::to_string(static_cast<long long>(args_.window_id)),
          "1",
          std::to_string(args_.width),
          std::to_string(args_.height),
          "1",
          args_.skyline_addon_path.empty() ? "stub" : args_.skyline_addon_path,
      };
    }
    config.present = &WindowHost::OnPresent;
    config.vsync = &WindowHost::OnVsync;
    config.semantics = &OnSemanticsUpdate;
    config.log_message = &LogMessage;
    config.user_data = this;

    try {
      engine_ = std::make_unique<embedder::RunningEngine>(config);
    } catch (const std::exception& error) {
      Fail(error.what());
      return;
    }
    if (use_aot_) {
      SKY_LOG(kInfo, kLog, "window %g: AOT snapshot %s", args_.window_id,
              aot_library_.c_str());
    }
    embedder::SetAccessibilityTarget(&loop_);
    SkylineSetAccessibilitySink(&embedder::RouteAccessibility);

    const FlutterEngineResult metrics_result =
        engine_->SendMetrics(args_.width, args_.height, pixel_ratio_);
    if (metrics_result != kSuccess) {
      embedder::ClearAccessibilityTarget(&loop_);
      engine_.reset();
      Fail("FlutterEngineSendWindowMetricsEvent failed: " +
           std::to_string(static_cast<int>(metrics_result)));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_ = true;
    }
    ready_cv_.notify_all();
    SKY_LOG(kInfo, kLog, "window %g ready (%zux%zu @%g, assets=%s)",
            args_.window_id,
            embedder::PhysicalPixels(args_.width, pixel_ratio_),
            embedder::PhysicalPixels(args_.height, pixel_ratio_), pixel_ratio_,
            assets_.c_str());

    while (loop_.RunOnce(engine_->get(), std::chrono::milliseconds(5))) {
    }
    // Return outstanding batons so shutdown does not wait on the vsync
    // service (matches the offline host's drain window).
    loop_.DrainVsync(engine_->get(), std::chrono::milliseconds(150));
    embedder::ClearAccessibilityTarget(&loop_);
    engine_.reset();
    SKY_LOG(kInfo, kLog, "window %g stopped", args_.window_id);
  }

  void Fail(const std::string& message) {
    SKY_LOG(kError, kLog, "window %g: %s", args_.window_id, message.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    init_error_ = message;
    ready_ = true;
    ready_cv_.notify_all();
  }

  static void OnVsync(void* user_data, intptr_t baton) {
    static_cast<WindowHost*>(user_data)->loop_.OnVsync(baton);
  }

  static bool OnPresent(void* user_data, const void* allocation,
                        size_t row_bytes, size_t height) {
    return static_cast<WindowHost*>(user_data)->Present(allocation, row_bytes,
                                                         height);
  }

  bool Present(const void* allocation, size_t row_bytes, size_t height) {
    if (allocation == nullptr || row_bytes < 4 || height == 0) return true;
    const uint32_t width = static_cast<uint32_t>(row_bytes / 4);
    const size_t frame_size = static_cast<size_t>(width) * height * 4;

    WindowBackend::RasterizedCallback callback;
    bool wrote = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (frame_buffer_ != nullptr &&
          frame_buffer_size_ >= SKYLINE_FRAME_HEADER_BYTES + frame_size) {
        auto* data = static_cast<uint8_t*>(frame_buffer_);
        embedder::CopyFrame(allocation, row_bytes, width, height,
                            data + SKYLINE_FRAME_HEADER_BYTES);
        embedder::WriteFrameHeader(data, ++frame_id_,
                                   static_cast<uint32_t>(frame_size));
        wrote = true;
        if (frame_id_ == 1) {
          SKY_LOG(kInfo, kLog, "window %g first frame %ux%zu (%zu bytes)",
                  args_.window_id, width, height, frame_size);
        }
      } else if (!warned_no_buffer_) {
        warned_no_buffer_ = true;
        SKY_LOG(kWarning, kLog,
                "window %g: frame buffer missing or too small (%zu < %zu); "
                "frames are dropped",
                args_.window_id, frame_buffer_size_,
                SKYLINE_FRAME_HEADER_BYTES + frame_size);
      }
      callback = callback_;
    }
    if (wrote && callback) callback();
    return true;
  }

  CreateWindowArgs args_;
  std::string assets_;
  std::string aot_library_;
  std::string icu_;
  bool use_aot_ = false;
  // Written before the platform thread starts; read-only afterwards.
  double pixel_ratio_ = 1.0;

  std::thread thread_;
  embedder::PlatformLoop loop_;
  // Created, used and destroyed on the platform thread.
  std::unique_ptr<embedder::RunningEngine> engine_;
  // Platform-thread-only single-touch state (see SendPointer).
  bool touch_down_ = false;

  std::mutex mutex_;
  std::condition_variable ready_cv_;
  bool ready_ = false;
  std::string init_error_;
  WindowBackend::RasterizedCallback callback_;
  void* frame_buffer_ = nullptr;
  size_t frame_buffer_size_ = 0;
  uint32_t frame_id_ = 0;
  bool warned_no_buffer_ = false;
};

class FlutterWindowBackend final : public WindowBackend {
 public:
  void CreateWindow(const CreateWindowArgs& args) override {
    if (args.bundle_path.empty()) {
      throw std::runtime_error("bundle_path is required");
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (windows_.count(args.window_id) != 0) {
        throw std::runtime_error("window already exists");
      }
    }
    auto window = std::make_unique<WindowHost>(args);
    if (args.frame_buffer != nullptr && args.frame_buffer_size > 0) {
      window->SetFrameBuffer(args.frame_buffer, args.frame_buffer_size);
    }
    window->Start();
    std::lock_guard<std::mutex> lock(mutex_);
    windows_[args.window_id] = std::move(window);
  }

  void DestroyWindow(double window_id) override {
    std::unique_ptr<WindowHost> window;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = windows_.find(window_id);
      if (found == windows_.end()) return;
      window = std::move(found->second);
      windows_.erase(found);
    }
    SKY_LOG(kInfo, kLog, "destroy window %g", window_id);
    window.reset();  // joins the platform thread outside the map lock
  }

  bool HasWindow(double window_id) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return windows_.count(window_id) != 0;
  }

  void SetRasterizedCallback(double window_id,
                             RasterizedCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = windows_.find(window_id);
    if (found == windows_.end()) {
      throw std::runtime_error("window_id: " +
                               std::to_string(static_cast<long long>(window_id)) +
                               " not found");
    }
    found->second->SetRasterizedCallback(std::move(callback));
  }

  void DispatchPointer(double window_id, PointerPhase phase, double x,
                       double y) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = windows_.find(window_id);
    if (found != windows_.end()) found->second->SendPointer(phase, x, y);
  }

  void DispatchWheel(double window_id, double x, double y, double delta_x,
                     double delta_y) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = windows_.find(window_id);
    if (found != windows_.end()) {
      found->second->SendWheel(x, y, delta_x, delta_y);
    }
  }

 private:
  mutable std::mutex mutex_;
  std::map<double, std::unique_ptr<WindowHost>> windows_;
};

}  // namespace

std::unique_ptr<WindowBackend> CreateFlutterWindowBackend() {
  return std::make_unique<FlutterWindowBackend>();
}

}  // namespace skyline

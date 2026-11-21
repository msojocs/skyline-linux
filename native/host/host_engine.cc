// Linux Flutter Engine backend for `skyline.node` (phase 2).
//
// The embedder API is not thread-safe across platform threads: the thread that
// calls `FlutterEngineRun` becomes the platform thread and must also service
// vsync batons and shut the engine down.  Each `CreateWindow` therefore starts
// its own platform thread and keeps it for the window's lifetime.  Presented
// frames are copied (BGRA -> RGBA) into the caller-supplied shared-memory data
// area and the Node callback is dispatched through a threadsafe function.

#include "host_engine.hh"

#include <flutter_embedder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skyline {
namespace {

constexpr size_t kFrameHeaderBytes = 8;
constexpr uint64_t kVsyncPeriodNanos = 16666667;  // 60 Hz hint.
constexpr int32_t kTouchDevice = 1;
constexpr int32_t kMouseDevice = 2;

bool FileExists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  std::fclose(file);
  return true;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

std::string ResolveAssets(const std::string& bundle_path) {
  if (const char* override_path = std::getenv("SKYLINE_ASSETS")) {
    if (override_path[0] != '\0') return override_path;
  }
  const std::string nested = JoinPath(bundle_path, "flutter_assets");
  if (FileExists(JoinPath(nested, "kernel_blob.bin"))) return nested;
  if (FileExists(JoinPath(bundle_path, "kernel_blob.bin"))) return bundle_path;
  return nested;
}

std::string ResolveAot(const std::string& bundle_path,
                       const std::string& assets) {
  if (const char* override_path = std::getenv("SKYLINE_AOT_LIBRARY")) {
    if (override_path[0] != '\0' && FileExists(override_path)) {
      return override_path;
    }
  }
  const std::string candidates[] = {
      JoinPath(bundle_path, "libapp.so"),
      JoinPath(assets, "libapp.so"),
      JoinPath(assets, "../libapp.so"),
  };
  for (const std::string& candidate : candidates) {
    if (FileExists(candidate)) return candidate;
  }
  return {};
}

std::string ResolveIcu(const std::string& bundle_path) {  const std::string candidates[] = {
      JoinPath(bundle_path, "icudtl.dat"),
      JoinPath(JoinPath(bundle_path, "flutter_assets"), "icudtl.dat"),
  };
  for (const std::string& candidate : candidates) {
    if (FileExists(candidate)) return candidate;
  }
  if (const char* override_path = std::getenv("SKYLINE_ICU")) {
    if (override_path[0] != '\0' && FileExists(override_path)) {
      return override_path;
    }
  }
#ifdef SKYLINE_DEFAULT_ICU
  if (FileExists(SKYLINE_DEFAULT_ICU)) return SKYLINE_DEFAULT_ICU;
#endif
  return {};
}

void PutU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

void LogMessage(const char* tag, const char* message, void* /*user_data*/) {
  std::fprintf(stderr, "[skyline-engine:%s] %s\n", tag != nullptr ? tag : "?",
               message != nullptr ? message : "");
}

class WindowHost {
 public:
  explicit WindowHost(CreateWindowArgs args) : args_(std::move(args)) {}
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
    const char* env_aot = std::getenv("SKYLINE_AOT_LIBRARY");
    use_aot_ = !has_kernel || (env_aot != nullptr && env_aot[0] != '\0');
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
    physical_width_ = static_cast<uint32_t>(std::ceil(width * dpr));
    physical_height_ = static_cast<uint32_t>(std::ceil(height * dpr));
    pixel_ratio_ = dpr;

    thread_ = std::thread([this]() { Run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_cv_.wait_for(lock, std::chrono::seconds(15),
                            [this]() { return ready_; })) {
      stop_ = true;
      lock.unlock();
      cv_.notify_all();
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  void SetRasterizedCallback(Engine::RasterizedCallback callback) {
    Engine::RasterizedCallback notify_now;
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

  // Queue a closure for the platform thread (the `Run` thread owns `engine_`
  // and is the only thread allowed to call the input embedder APIs).
  void PostPlatformTask(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || !ready_) return;
      platform_tasks_.push_back(std::move(task));
    }
    cv_.notify_all();
  }

  // DevTools simulator input.  The caller passes logical (CSS) pixels; Flutter
  // expects physical pixels for the implicit view.
  //
  // `FlutterEngineSendPointerEvent` is platform-thread-only (the window's
  // `Run` thread).  The NAPI/JS thread must not call it directly: doing so
  // deadlocks the appservice webview on touch and trips the engine's
  // weak_ptr thread check.  Marshal the event onto the platform thread and
  // keep the mailbox FIFO so down/move/up stay ordered.
  void SendPointer(Engine::PointerPhase phase, double x, double y) {
    double dpr = 1.0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dpr = pixel_ratio_;
    }
    FlutterPointerEvent event = {};
    event.struct_size = sizeof(event);
    event.timestamp = FlutterEngineGetCurrentTime();
    event.x = x * dpr;
    event.y = y * dpr;
    // Touch and mouse need distinct device ids: the engine keeps one pointer
    // state per device, so hovering with a "touch" device while it is down
    // trips the converter's !is_down checks.
    event.device = phase == Engine::PointerPhase::kHover ? kMouseDevice
                                                        : kTouchDevice;
    event.view_id = 0;
    event.signal_kind = kFlutterPointerSignalKindNone;
    switch (phase) {
      case Engine::PointerPhase::kDown:
        event.phase = kDown;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case Engine::PointerPhase::kMove:
        event.phase = kMove;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case Engine::PointerPhase::kUp:
        event.phase = kUp;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case Engine::PointerPhase::kCancel:
        event.phase = kCancel;
        event.device_kind = kFlutterPointerDeviceKindTouch;
        break;
      case Engine::PointerPhase::kHover:
        event.phase = kHover;
        event.device_kind = kFlutterPointerDeviceKindMouse;
        break;
    }
    PostPlatformTask([this, event, phase]() {
      if (engine_ == nullptr) return;
      // Synthetic touch sequences from the simulator can repeat a phase (for
      // example a click handler firing after a touch sequence).  The engine
      // aborts the whole process on a duplicate down / orphan move / up, so
      // keep the single-touch state machine coherent here.
      if (event.device_kind == kFlutterPointerDeviceKindTouch) {
        if (phase == Engine::PointerPhase::kDown) {
          if (touch_down_) return;
          touch_down_ = true;
        } else if (phase == Engine::PointerPhase::kUp ||
                   phase == Engine::PointerPhase::kCancel) {
          if (!touch_down_) return;
          touch_down_ = false;
        } else if (phase == Engine::PointerPhase::kMove) {
          if (!touch_down_) return;
        }
      }
      FlutterEngineSendPointerEvent(engine_, &event, 1);
    });
  }

  void SendWheel(double x, double y, double delta_x, double delta_y) {
    double dpr = 1.0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dpr = pixel_ratio_;
    }
    FlutterPointerEvent event = {};
    event.struct_size = sizeof(event);
    event.phase = kMove;
    event.timestamp = FlutterEngineGetCurrentTime();
    event.x = x * dpr;
    event.y = y * dpr;
    event.device = kMouseDevice;
    event.view_id = 0;
    event.device_kind = kFlutterPointerDeviceKindMouse;
    event.signal_kind = kFlutterPointerSignalKindScroll;
    event.scroll_delta_x = delta_x;
    event.scroll_delta_y = delta_y;
    PostPlatformTask([this, event]() {
      if (engine_ != nullptr) FlutterEngineSendPointerEvent(engine_, &event, 1);
    });
  }

 private:
  void Run() {
    FlutterRendererConfig renderer = {};
    renderer.type = kSoftware;
    renderer.software.struct_size = sizeof(FlutterSoftwareRendererConfig);
    renderer.software.surface_present_callback = &WindowHost::OnPresent;

    FlutterProjectArgs project = {};
    project.struct_size = sizeof(FlutterProjectArgs);
    project.assets_path = assets_.c_str();
    project.icu_data_path = icu_.c_str();
    if (use_aot_) {
      FlutterEngineAOTDataSource source = {};
      source.type = kFlutterEngineAOTDataSourceTypeElfPath;
      source.elf_path = aot_library_.c_str();
      if (FlutterEngineCreateAOTData(&source, &aot_data_) != kSuccess ||
          aot_data_ == nullptr) {
        Fail("cannot load AOT snapshot: " + aot_library_);
        return;
      }
      project.aot_data = aot_data_;
      std::fprintf(stderr, "[skyline-engine] window %g: AOT snapshot %s\n",
                   args_.window_id, aot_library_.c_str());
    }
    project.vsync_callback = &WindowHost::OnVsync;
    project.log_message_callback = &LogMessage;
    project.log_tag = "skyline_host";
    project.shutdown_dart_vm_when_done = true;
    const char* command_line[] = {"skyline_engine", "--disable-vm-service"};
    project.command_line_argc = 2;
    project.command_line_argv = command_line;

    // The DevTools' Skyline Dart entrypoint (`package:sample/devtools.dart
    // main`) expects [window id, runtime holder, width, height, createRuntime,
    // addon path].  Plain test apps use a zero-argument main, so only pass
    // these for the DevTools packaging.  The DevTools always loads the addon
    // from its own `node_modules/skyline-addon/` directory, which is enough to
    // recognize it without requiring environment variables.
    const bool devtools_entrypoint =
        std::getenv("SKYLINE_DEVTOOLS") != nullptr ||
        args_.bundle_path.find("skyline-addon") != std::string::npos ||
        args_.skyline_addon_path.find("skyline-addon") != std::string::npos;
    std::vector<std::string> dart_args;
    if (devtools_entrypoint) {
      dart_args.push_back(
          std::to_string(static_cast<long long>(args_.window_id)));
      dart_args.push_back("1");
      dart_args.push_back(std::to_string(args_.width));
      dart_args.push_back(std::to_string(args_.height));
      dart_args.push_back("1");
      dart_args.push_back(args_.skyline_addon_path.empty()
                              ? "stub"
                              : args_.skyline_addon_path);
    }
    std::vector<const char*> dart_argv;
    dart_argv.reserve(dart_args.size());
    for (const std::string& value : dart_args) {
      dart_argv.push_back(value.c_str());
    }
    if (!dart_argv.empty()) {
      project.dart_entrypoint_argc = static_cast<int>(dart_argv.size());
      project.dart_entrypoint_argv = dart_argv.data();
    }

    FlutterEngine engine = nullptr;
    const FlutterEngineResult run_result = FlutterEngineRun(
        FLUTTER_ENGINE_VERSION, &renderer, &project, this, &engine);
    if (run_result != kSuccess || engine == nullptr) {
      if (aot_data_ != nullptr) {
        FlutterEngineCollectAOTData(aot_data_);
        aot_data_ = nullptr;
      }
      Fail("FlutterEngineRun failed: " +
           std::to_string(static_cast<int>(run_result)));
      return;
    }
    engine_ = engine;

    FlutterWindowMetricsEvent metrics = {};
    metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    metrics.width = physical_width_;
    metrics.height = physical_height_;
    metrics.pixel_ratio = pixel_ratio_;
    metrics.view_id = 0;  // kFlutterImplicitViewId
    const FlutterEngineResult metrics_result =
        FlutterEngineSendWindowMetricsEvent(engine, &metrics);
    if (metrics_result != kSuccess) {
      Fail("FlutterEngineSendWindowMetricsEvent failed: " +
           std::to_string(static_cast<int>(metrics_result)));
      FlutterEngineShutdown(engine);
      engine_ = nullptr;
      if (aot_data_ != nullptr) {
        FlutterEngineCollectAOTData(aot_data_);
        aot_data_ = nullptr;
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_ = true;
    }
    ready_cv_.notify_all();
    std::fprintf(stderr,
                 "[skyline-engine] window %g ready (%ux%u @%g, assets=%s)\n",
                 args_.window_id, physical_width_, physical_height_,
                 pixel_ratio_, assets_.c_str());

    while (true) {
      std::deque<intptr_t> batons;
      std::deque<std::function<void()>> tasks;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
          return stop_ || !batons_.empty() || !platform_tasks_.empty();
        });
        if (stop_ && batons_.empty() && platform_tasks_.empty()) break;
        batons.swap(batons_);
        tasks.swap(platform_tasks_);
      }
      for (auto& task : tasks) task();
      for (intptr_t baton : batons) {
        const uint64_t now = FlutterEngineGetCurrentTime();
        FlutterEngineOnVsync(engine, baton, now, now + kVsyncPeriodNanos);
      }
    }  // loop

    // Return outstanding batons so shutdown does not wait on the vsync
    // service (matches the offline host's drain window).
    const auto drain_until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < drain_until) {
      std::deque<intptr_t> pending;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (batons_.empty()) {
          cv_.wait_for(lock, std::chrono::milliseconds(5));
        }
        pending.swap(batons_);
      }
      for (intptr_t baton : pending) {
        const uint64_t now = FlutterEngineGetCurrentTime();
        FlutterEngineOnVsync(engine, baton, now, now + kVsyncPeriodNanos);
      }
    }

    FlutterEngineShutdown(engine);
    engine_ = nullptr;
    if (aot_data_ != nullptr) {
      FlutterEngineCollectAOTData(aot_data_);
      aot_data_ = nullptr;
    }
    std::fprintf(stderr, "[skyline-engine] window %g stopped\n",
                 args_.window_id);
  }

  void Fail(const std::string& message) {
    std::fprintf(stderr, "[skyline-engine] window %g: %s\n", args_.window_id,
                 message.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    init_error_ = message;
    ready_ = true;
    ready_cv_.notify_all();
  }

  static void OnVsync(void* user_data, intptr_t baton) {
    auto* self = static_cast<WindowHost*>(user_data);
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->batons_.push_back(baton);
    }
    self->cv_.notify_all();
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

    Engine::RasterizedCallback callback;
    bool wrote = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (frame_buffer_ != nullptr &&
          frame_buffer_size_ >= kFrameHeaderBytes + frame_size) {
        auto* destination = static_cast<uint8_t*>(frame_buffer_) +
                            kFrameHeaderBytes;
        const auto* source = static_cast<const uint8_t*>(allocation);
        // Skia's N32 software surface is BGRA on little-endian Linux; the
        // contract wants RGBA8.
        for (size_t y = 0; y < height; ++y) {
          const uint8_t* row_src = source + y * row_bytes;
          uint8_t* row_dst = destination + y * width * 4;
          for (uint32_t x = 0; x < width; ++x) {
            row_dst[x * 4 + 0] = row_src[x * 4 + 2];
            row_dst[x * 4 + 1] = row_src[x * 4 + 1];
            row_dst[x * 4 + 2] = row_src[x * 4 + 0];
            row_dst[x * 4 + 3] = row_src[x * 4 + 3];
          }
        }
        auto* header = static_cast<uint8_t*>(frame_buffer_);
        PutU32(header + 4, static_cast<uint32_t>(frame_size));
        std::atomic_thread_fence(std::memory_order_release);
        PutU32(header, ++frame_id_);
        wrote = true;
        if (frame_id_ == 1) {
          std::fprintf(stderr,
                       "[skyline-engine] window %g first frame %ux%zu "
                       "(%zu bytes)\n",
                       args_.window_id, width, height, frame_size);
        }
      } else if (!warned_no_buffer_) {
        warned_no_buffer_ = true;
        std::fprintf(stderr,
                     "[skyline-engine] window %g: frame buffer missing or too "
                     "small (%zu < %zu); frames are dropped\n",
                     args_.window_id, frame_buffer_size_,
                     kFrameHeaderBytes + frame_size);
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
  FlutterEngineAOTData aot_data_ = nullptr;
  uint32_t physical_width_ = 0;
  uint32_t physical_height_ = 0;
  double pixel_ratio_ = 1.0;

  std::thread thread_;
  FlutterEngine engine_ = nullptr;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable ready_cv_;
  std::deque<intptr_t> batons_;
  std::deque<std::function<void()>> platform_tasks_;
  // Platform-thread-only single-touch state (see SendPointer).
  bool touch_down_ = false;
  bool ready_ = false;
  bool stop_ = false;
  std::string init_error_;
  Engine::RasterizedCallback callback_;

  void* frame_buffer_ = nullptr;
  size_t frame_buffer_size_ = 0;
  uint32_t frame_id_ = 0;
  bool warned_no_buffer_ = false;
};

class HostEngine final : public Engine {
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
    std::fprintf(stderr, "[skyline-engine] destroy window %g\n", window_id);
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

std::unique_ptr<Engine> CreateHostEngine() {
  return std::make_unique<HostEngine>();
}

}  // namespace skyline

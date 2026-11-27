#include "embedder/embedder.hh"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "skyline/runtime_api.h"

namespace skyline::embedder {
namespace {

void PutU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

std::mutex g_accessibility_mutex;
PlatformLoop* g_accessibility_target = nullptr;

}  // namespace

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

std::string FindAotLibrary(const std::string& bundle,
                           const std::string& assets) {
  for (const std::string& candidate :
       {JoinPath(bundle, "libapp.so"), JoinPath(assets, "../libapp.so"),
        JoinPath(assets, "libapp.so")}) {
    if (FileExists(candidate)) return candidate;
  }
  return {};
}

size_t PhysicalPixels(double logical, double pixel_ratio) {
  return static_cast<size_t>(std::ceil(logical * pixel_ratio));
}

void CopyFrame(const void* source, size_t row_bytes, uint32_t width,
               size_t height, void* destination, bool source_is_rgba) {
  const auto* src = static_cast<const uint8_t*>(source);
  auto* dst = static_cast<uint8_t*>(destination);
  const size_t dst_row_bytes = static_cast<size_t>(width) * 4;
  for (size_t y = 0; y < height; ++y) {
    const uint8_t* row_src = src + y * row_bytes;
    uint8_t* row_dst = dst + y * dst_row_bytes;
    if (source_is_rgba) {
      std::memcpy(row_dst, row_src, dst_row_bytes);
      continue;
    }
    for (uint32_t x = 0; x < width; ++x) {
      row_dst[x * 4 + 0] = row_src[x * 4 + 2];
      row_dst[x * 4 + 1] = row_src[x * 4 + 1];
      row_dst[x * 4 + 2] = row_src[x * 4 + 0];
      row_dst[x * 4 + 3] = row_src[x * 4 + 3];
    }
  }
}

void WriteFrameHeader(void* data_area, uint32_t frame_id,
                      uint32_t frame_size) {
  auto* header = static_cast<uint8_t*>(data_area);
  PutU32(header + 4, frame_size);
  std::atomic_thread_fence(std::memory_order_release);
  PutU32(header, frame_id);
}

void LogSemanticsUpdate(const char* prefix,
                        const FlutterSemanticsUpdate2* update) {
  if (update == nullptr || std::getenv("SKYLINE_APP_SERVICE_DEBUG") == nullptr) {
    return;
  }
  for (size_t i = 0; i < update->node_count; i++) {
    const FlutterSemanticsNode2* node = update->nodes[i];
    if (node == nullptr) continue;
    const FlutterSemanticsFlags* flags = node->flags2;
    std::fprintf(stderr,
                 "%s semantics node: id=%d label=\"%s\" value=\"%s\" "
                 "button=%d textField=%d header=%d children=%zu "
                 "rect=(%g,%g %gx%g)\n",
                 prefix, node->id, node->label != nullptr ? node->label : "",
                 node->value != nullptr ? node->value : "",
                 flags != nullptr && flags->is_button ? 1 : 0,
                 flags != nullptr && flags->is_text_field ? 1 : 0,
                 flags != nullptr && flags->is_header ? 1 : 0,
                 node->child_count, node->rect.left, node->rect.top,
                 node->rect.right - node->rect.left,
                 node->rect.bottom - node->rect.top);
  }
}

// -- RunningEngine ---------------------------------------------------------------

RunningEngine::RunningEngine(const EngineConfig& config) {
  FlutterRendererConfig renderer = {};
  renderer.type = kSoftware;
  renderer.software.struct_size = sizeof(FlutterSoftwareRendererConfig);
  renderer.software.surface_present_callback = config.present;

  FlutterProjectArgs project = {};
  project.struct_size = sizeof(FlutterProjectArgs);
  project.assets_path = config.assets.c_str();
  project.icu_data_path = config.icu.c_str();
  if (!config.aot_library.empty()) {
    FlutterEngineAOTDataSource source = {};
    source.type = kFlutterEngineAOTDataSourceTypeElfPath;
    source.elf_path = config.aot_library.c_str();
    if (FlutterEngineCreateAOTData(&source, &aot_data_) != kSuccess ||
        aot_data_ == nullptr) {
      aot_data_ = nullptr;
      throw std::runtime_error("cannot load AOT snapshot: " +
                               config.aot_library);
    }
    project.aot_data = aot_data_;
  }
  project.vsync_callback = config.vsync;
  project.log_message_callback = config.log_message;
  project.log_tag = "skyline_host";
  project.shutdown_dart_vm_when_done = true;
  project.update_semantics_callback2 = config.semantics;
  if (!config.entrypoint.empty()) {
    project.custom_dart_entrypoint = config.entrypoint.c_str();
  }
  std::vector<const char*> entrypoint_argv;
  for (const std::string& value : config.entrypoint_args) {
    entrypoint_argv.push_back(value.c_str());
  }
  if (!entrypoint_argv.empty()) {
    project.dart_entrypoint_argc = static_cast<int>(entrypoint_argv.size());
    project.dart_entrypoint_argv = entrypoint_argv.data();
  }
  const char* command_line[] = {config.program, "--disable-vm-service"};
  project.command_line_argc = 2;
  project.command_line_argv = command_line;

  // The engine copies the strings and argument vectors it keeps.
  const FlutterEngineResult result = FlutterEngineRun(
      FLUTTER_ENGINE_VERSION, &renderer, &project, config.user_data, &engine_);
  if (result != kSuccess || engine_ == nullptr) {
    engine_ = nullptr;
    Shutdown();
    throw std::runtime_error("FlutterEngineRun failed: " +
                             std::to_string(static_cast<int>(result)));
  }
}

FlutterEngineResult RunningEngine::SendMetrics(double width, double height,
                                               double pixel_ratio) {
  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
  metrics.width = PhysicalPixels(width, pixel_ratio);
  metrics.height = PhysicalPixels(height, pixel_ratio);
  metrics.pixel_ratio = pixel_ratio;
  metrics.view_id = 0;  // kFlutterImplicitViewId
  return FlutterEngineSendWindowMetricsEvent(engine_, &metrics);
}

FlutterEngineResult RunningEngine::Shutdown() {
  FlutterEngineResult result = kSuccess;
  if (engine_ != nullptr) {
    result = FlutterEngineShutdown(engine_);
    engine_ = nullptr;
  }
  if (aot_data_ != nullptr) {
    FlutterEngineCollectAOTData(aot_data_);
    aot_data_ = nullptr;
  }
  return result;
}

// -- PlatformLoop ------------------------------------------------------------------

void PlatformLoop::OnVsync(intptr_t baton) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batons_.push_back(baton);
    ++vsync_count_;
  }
  cv_.notify_all();
}

void PlatformLoop::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    tasks_.push_back(std::move(task));
  }
  cv_.notify_all();
}

void PlatformLoop::PostAccessibility(int32_t kind, int32_t value) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accessibility_.emplace_back(kind, value);
  }
  cv_.notify_all();
}

void PlatformLoop::Wake() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    woken_ = true;
  }
  cv_.notify_all();
}

void PlatformLoop::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  cv_.notify_all();
}

bool PlatformLoop::RunOnce(FlutterEngine engine,
                           std::chrono::milliseconds timeout) {
  std::deque<intptr_t> batons;
  std::deque<std::function<void()>> tasks;
  std::deque<std::pair<int32_t, int32_t>> accessibility;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this]() {
      return stopped_ || woken_ || !batons_.empty() || !tasks_.empty() ||
             !accessibility_.empty();
    });
    if (stopped_ && batons_.empty() && tasks_.empty()) return false;
    woken_ = false;
    batons.swap(batons_);
    tasks.swap(tasks_);
    accessibility.swap(accessibility_);
  }
  // The engine posts semantics updates (and other platform callbacks) to the
  // platform task runner.  These hosts run no fml message loop, so drain the
  // pending tasks explicitly.
  __FlutterEngineFlushPendingTasksNow();
  for (const auto& [kind, value] : accessibility) {
    if (kind == SKYLINE_ACCESSIBILITY_FEATURES) {
      const FlutterAccessibilityFeature features =
          value != 0 ? kFlutterAccessibilityFeatureAccessibleNavigation
                     : static_cast<FlutterAccessibilityFeature>(0);
      const FlutterEngineResult result =
          FlutterEngineUpdateAccessibilityFeatures(engine, features);
      std::fprintf(stderr, "%s accessibility features: %d (result=%d)\n",
                   log_prefix_.c_str(), static_cast<int>(features),
                   static_cast<int>(result));
    } else {
      const FlutterEngineResult result =
          FlutterEngineUpdateSemanticsEnabled(engine, value != 0);
      std::fprintf(stderr, "%s semantics enabled: %d (result=%d)\n",
                   log_prefix_.c_str(), value != 0 ? 1 : 0,
                   static_cast<int>(result));
    }
  }
  for (auto& task : tasks) task();
  ReturnBatons(engine, batons);
  return true;
}

void PlatformLoop::DrainVsync(FlutterEngine engine,
                              std::chrono::milliseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
    std::deque<intptr_t> batons;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (batons_.empty()) cv_.wait_for(lock, std::chrono::milliseconds(5));
      batons.swap(batons_);
    }
    ReturnBatons(engine, batons);
  }
  std::deque<intptr_t> batons;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batons.swap(batons_);
  }
  ReturnBatons(engine, batons);
}

uint32_t PlatformLoop::vsync_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return vsync_count_;
}

size_t PlatformLoop::pending_batons() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return batons_.size();
}

void PlatformLoop::ReturnBatons(FlutterEngine engine,
                                const std::deque<intptr_t>& batons) {
  for (intptr_t baton : batons) {
    const uint64_t now = FlutterEngineGetCurrentTime();
    FlutterEngineOnVsync(engine, baton, now, now + kVsyncPeriodNanos);
  }
}

// -- Accessibility routing ------------------------------------------------------------

void SetAccessibilityTarget(PlatformLoop* loop) {
  std::lock_guard<std::mutex> lock(g_accessibility_mutex);
  g_accessibility_target = loop;
}

void ClearAccessibilityTarget(PlatformLoop* loop) {
  std::lock_guard<std::mutex> lock(g_accessibility_mutex);
  if (g_accessibility_target == loop) g_accessibility_target = nullptr;
}

void RouteAccessibility(int32_t kind, int32_t value) {
  std::lock_guard<std::mutex> lock(g_accessibility_mutex);
  if (g_accessibility_target != nullptr) {
    g_accessibility_target->PostAccessibility(kind, value);
  }
}

}  // namespace skyline::embedder

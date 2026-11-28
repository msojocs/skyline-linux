#pragma once

// Flutter embedder plumbing shared by the standalone skyline_host
// (host/main.cc) and skyline.node's window backend
// (host/flutter_window_backend.cc): engine resources, engine start/shutdown,
// the platform-thread loop (vsync batons, platform tasks, accessibility), frame
// conversion and semantics logging.

#include <flutter_embedder.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace skyline::embedder {

// Vsync interval reported with every returned baton (60 Hz).
constexpr uint64_t kVsyncPeriodNanos = 16666667;

bool FileExists(const std::string& path);
std::string JoinPath(const std::string& dir, const std::string& name);

// The AOT snapshot (`libapp.so`) of a bundle: next to the bundle's assets
// directory first, then inside it.  Empty when there is none.
std::string FindAotLibrary(const std::string& bundle,
                           const std::string& assets);

// Physical pixels for a logical extent (rounded up, as the DevTools does).
size_t PhysicalPixels(double logical, double pixel_ratio);

// Copies a software surface into tightly packed RGBA8 rows.  Skia's N32
// surface is BGRA on little-endian Linux, so red and blue are swapped unless
// the surface is already RGBA.
void CopyFrame(const void* source, size_t row_bytes, uint32_t width,
               size_t height, void* destination, bool source_is_rgba = false);

// Publishes the frame header at the start of a data area (frame protocol):
// the size first, then the id, so a reader that sees the id sees its size.
void WriteFrameHeader(void* data_area, uint32_t frame_id, uint32_t frame_size);

// Logs every node of a semantics update ("semantics node: ...") at debug
// level under `category`.
void LogSemanticsUpdate(const char* category,
                        const FlutterSemanticsUpdate2* update);

// Forwards the engine's log callback (Dart print/debugPrint, which also
// carries Flutter error reports) as "[<category>:<tag>] message".  It is
// application output, so it is logged at warning level and shown by default.
void LogEngineMessage(const char* category, const char* tag,
                      const char* message);

struct EngineConfig {
  std::string assets;
  std::string icu;
  // libapp.so to run; empty runs the JIT kernel in `assets`.
  std::string aot_library;
  // Custom Dart entrypoint; empty runs `main`.
  std::string entrypoint;
  std::vector<std::string> entrypoint_args;
  // argv[0] of the engine command line.
  const char* program = "skyline_engine";
  // Every callback receives `user_data`.
  SoftwareSurfacePresentCallback present = nullptr;
  VsyncCallback vsync = nullptr;
  FlutterUpdateSemanticsCallback2 semantics = nullptr;
  FlutterLogMessageCallback log_message = nullptr;
  void* user_data = nullptr;
};

// A running engine and its AOT data.  The thread that constructs it becomes
// the engine's platform thread: input, metrics and shutdown must happen there.
class RunningEngine {
 public:
  // Starts the engine; throws std::runtime_error naming the failing step.
  explicit RunningEngine(const EngineConfig& config);
  ~RunningEngine() { Shutdown(); }

  RunningEngine(const RunningEngine&) = delete;
  RunningEngine& operator=(const RunningEngine&) = delete;

  FlutterEngine get() const { return engine_; }
  // Sizes the implicit view from a logical size.
  FlutterEngineResult SendMetrics(double width, double height,
                                  double pixel_ratio);
  // Shuts the engine down and releases the AOT data (idempotent).
  FlutterEngineResult Shutdown();

 private:
  FlutterEngine engine_ = nullptr;
  FlutterEngineAOTData aot_data_ = nullptr;
};

// The platform-thread duties of one engine.  The embedder API must be called
// on the thread that ran FlutterEngineRun; other threads queue work here and
// the platform thread services it with RunOnce().
class PlatformLoop {
 public:
  // Accessibility changes are logged under `category`, each message
  // starting with `label` ("window 1 ", ...).
  PlatformLoop(const char* category, std::string label)
      : category_(category), label_(std::move(label)) {}

  // Engine vsync callback target; any thread.
  void OnVsync(intptr_t baton);
  // Any thread: runs `task` on the platform thread; dropped once stopped.
  void Post(std::function<void()> task);
  // Any thread: an accessibility request from the embedded runtime
  // (SkylineAccessibilitySink kinds), applied on the platform thread.
  void PostAccessibility(int32_t kind, int32_t value);
  // Any thread: ends the current RunOnce() wait early.
  void Wake();
  // Any thread: RunOnce() returns false once the queued work is done.
  void Stop();

  // Platform thread: waits up to `timeout` for work, then flushes the
  // engine's pending platform tasks, applies accessibility requests, runs
  // posted tasks and returns vsync batons.  Returns false when stopped.
  bool RunOnce(FlutterEngine engine, std::chrono::milliseconds timeout);
  // Platform thread: keeps returning batons for `duration` so a shutdown does
  // not wait on the vsync service.
  void DrainVsync(FlutterEngine engine, std::chrono::milliseconds duration);

  uint32_t vsync_count() const;
  size_t pending_batons() const;

 private:
  void ReturnBatons(FlutterEngine engine, const std::deque<intptr_t>& batons);

  const char* category_;
  const std::string label_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<intptr_t> batons_;
  std::deque<std::function<void()>> tasks_;
  std::deque<std::pair<int32_t, int32_t>> accessibility_;
  uint32_t vsync_count_ = 0;
  bool woken_ = false;
  bool stopped_ = false;
};

// The embedded runtime reports accessibility through one process-wide sink
// (SkylineSetAccessibilitySink); it is routed to the loop registered here.
void SetAccessibilityTarget(PlatformLoop* loop);
// Unregisters `loop` if it is the current target.
void ClearAccessibilityTarget(PlatformLoop* loop);
// The sink to install with SkylineSetAccessibilitySink.
void RouteAccessibility(int32_t kind, int32_t value);

}  // namespace skyline::embedder

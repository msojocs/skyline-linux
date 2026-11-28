#include "skyline/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include "skyline/config.h"

namespace skyline::log {
namespace {

// Thresholds as integers: a message reaches a sink when its level is at least
// the sink's threshold.  kOff disables a sink.
constexpr int kOff = static_cast<int>(Level::kError) + 1;

int ConfiguredThreshold() {
  if (config::Flag(config::kAppServiceDebug)) {
    return static_cast<int>(Level::kDebug);
  }
  const std::string& name = config::String(config::kLogLevel);
  if (name == "debug") return static_cast<int>(Level::kDebug);
  if (name == "info") return static_cast<int>(Level::kInfo);
  if (name == "error") return static_cast<int>(Level::kError);
  if (name == "off") return kOff;
  return static_cast<int>(Level::kWarning);
}

std::atomic<int>& Threshold() {
  static std::atomic<int> threshold{ConfiguredThreshold()};
  return threshold;
}

int StderrThreshold() { return Threshold().load(std::memory_order_relaxed); }

struct FileSink {
  std::mutex mutex;
  std::string path;
  size_t max_bytes = 0;
  std::FILE* file = nullptr;
};

FileSink& File() {
  static auto* sink = new FileSink();  // never destroyed
  return *sink;
}

std::atomic<int> g_file_threshold{kOff};

// Appends `line` and rotates the file once it outgrows the limit.  Called
// with the sink mutex held.
void AppendToFile(FileSink& sink, const std::string& line) {
  if (sink.file == nullptr) return;
  std::fprintf(sink.file, "[pid %d] %s", static_cast<int>(::getpid()),
               line.c_str());
  std::fflush(sink.file);
  const long size = std::ftell(sink.file);
  if (size < 0 || static_cast<size_t>(size) <= sink.max_bytes) return;
  std::fclose(sink.file);
  std::rename(sink.path.c_str(), (sink.path + ".1").c_str());
  sink.file = std::fopen(sink.path.c_str(), "a");
}

}  // namespace

bool Enabled(Level level) {
  const int value = static_cast<int>(level);
  return value >= StderrThreshold() ||
         value >= g_file_threshold.load(std::memory_order_relaxed);
}

void Write(Level level, const char* category, const char* format, ...) {
  char stack[1024];
  va_list args;
  va_start(args, format);
  const int length = std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  std::string message;
  if (length < 0) {
    message = format;
  } else if (static_cast<size_t>(length) < sizeof(stack)) {
    message.assign(stack, static_cast<size_t>(length));
  } else {
    std::vector<char> heap(static_cast<size_t>(length) + 1);
    va_start(args, format);
    std::vsnprintf(heap.data(), heap.size(), format, args);
    va_end(args);
    message.assign(heap.data(), static_cast<size_t>(length));
  }
  const std::string line =
      "[" + std::string(category) + "] " + message + "\n";
  const int value = static_cast<int>(level);
  // One write per line keeps lines from different threads apart.
  if (value >= StderrThreshold()) {
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
  if (value >= g_file_threshold.load(std::memory_order_relaxed)) {
    FileSink& sink = File();
    std::lock_guard<std::mutex> lock(sink.mutex);
    AppendToFile(sink, line);
  }
}

void LowerThreshold(Level level) {
  std::atomic<int>& threshold = Threshold();
  int current = threshold.load();
  while (static_cast<int>(level) < current &&
         !threshold.compare_exchange_weak(current, static_cast<int>(level))) {
  }
}

void SetFileSink(const char* path, size_t max_bytes, Level min_level) {
  FileSink& sink = File();
  std::lock_guard<std::mutex> lock(sink.mutex);
  if (sink.file != nullptr) {
    std::fclose(sink.file);
    sink.file = nullptr;
  }
  sink.path = path != nullptr ? path : "";
  sink.max_bytes = max_bytes;
  if (!sink.path.empty()) sink.file = std::fopen(sink.path.c_str(), "a");
  g_file_threshold.store(sink.file != nullptr ? static_cast<int>(min_level)
                                              : kOff);
}

}  // namespace skyline::log

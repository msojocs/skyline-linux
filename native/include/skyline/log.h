#pragma once

// Leveled logging shared by the native components (runtime, skyline.node,
// skyline_host, sharedMemory.node).
//
//   SKY_LOG(kInfo, "skyline-runtime", "live ops: %zu bytes", size);
//
// writes "[skyline-runtime] live ops: 12 bytes" to stderr when the level is
// enabled; the arguments are not evaluated otherwise.  SKYLINE_LOG_LEVEL
// selects the threshold (default: warning); SKYLINE_APP_SERVICE_DEBUG lowers
// it to debug.  Each binary has its own copy of this state.

#include <cstddef>

namespace skyline::log {

enum class Level : int {
  kDebug = 0,
  kInfo = 1,
  kWarning = 2,
  kError = 3,
};

// Whether a message of `level` reaches any sink.
bool Enabled(Level level);

// Writes one line to the sinks whose threshold `level` reaches; use SKY_LOG,
// which checks Enabled() first.
void Write(Level level, const char* category, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

// Lowers this binary's stderr threshold to at most `level` (a component's own
// verbosity switch, e.g. SKYLINE_SHARED_MEMORY_LOG).
void LowerThreshold(Level level);

// Additionally appends messages of `min_level` and above to `path`, each
// prefixed with the process id.  The file is rotated to `<path>.1` once it
// exceeds `max_bytes`.  An empty path disables the file sink.
void SetFileSink(const char* path, size_t max_bytes, Level min_level);

}  // namespace skyline::log

#define SKY_LOG_ENABLED(level) \
  (::skyline::log::Enabled(::skyline::log::Level::level))

#define SKY_LOG(level, category, ...)                                       \
  do {                                                                      \
    if (SKY_LOG_ENABLED(level)) {                                           \
      ::skyline::log::Write(::skyline::log::Level::level, category,         \
                            __VA_ARGS__);                                   \
    }                                                                       \
  } while (0)

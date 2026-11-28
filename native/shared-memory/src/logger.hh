#ifndef __LOGGER_HH__
#define __LOGGER_HH__

// Logger for the sharedMemory addon.
//
// The upstream module used spdlog; this repository builds without external
// packages, so the same call sites take printf-style formats and go through
// the shared native log (skyline/log.h) under the "sharedMemory" category.
// SKYLINE_SHARED_MEMORY_LOG=1 shows every level; otherwise SKYLINE_LOG_LEVEL
// applies (warning and above by default).

#include <cstdarg>
#include <memory>

#include "skyline/log.h"

namespace Logger {

class Logger {
 public:
  void debug(const char* format, ...) const
      __attribute__((format(printf, 2, 3)));
  void info(const char* format, ...) const
      __attribute__((format(printf, 2, 3)));
  void warn(const char* format, ...) const
      __attribute__((format(printf, 2, 3)));
  void error(const char* format, ...) const
      __attribute__((format(printf, 2, 3)));

 private:
  static void Emit(skyline::log::Level level, const char* format,
                   va_list args);
};

extern std::shared_ptr<Logger> logger;

void Init();

}  // namespace Logger

#endif

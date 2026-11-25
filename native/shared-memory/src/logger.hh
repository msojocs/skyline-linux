#ifndef __LOGGER_HH__
#define __LOGGER_HH__

// Minimal logger for the sharedMemory addon.
//
// The upstream module used spdlog, which dragged in vcpkg.  This repository
// builds without external packages, so the addon keeps the same call sites but
// routes them to an opt-in stderr sink:
//
//   SKYLINE_SHARED_MEMORY_LOG=1 ./bin/wechat-devtools
//
// Messages are emitted verbatim; the upstream sources mix fmt ("{}") and
// printf ("%s") placeholders, so arguments are intentionally ignored.

#include <memory>

namespace Logger {

class Logger {
 public:
  template <typename... Args>
  void debug(const char* message, Args&&...) const {
    Emit("debug", message);
  }

  template <typename... Args>
  void info(const char* message, Args&&...) const {
    Emit("info", message);
  }

  template <typename... Args>
  void warn(const char* message, Args&&...) const {
    Emit("warn", message);
  }

  template <typename... Args>
  void error(const char* message, Args&&...) const {
    Emit("error", message);
  }

 private:
  static void Emit(const char* level, const char* message);
};

extern std::shared_ptr<Logger> logger;

void Init();

}  // namespace Logger

#endif

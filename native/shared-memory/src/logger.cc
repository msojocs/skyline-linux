#include "logger.hh"

#include <cstdio>
#include <string>
#include <vector>

#include "skyline/config.h"

namespace Logger {

std::shared_ptr<Logger> logger;

void Logger::Emit(skyline::log::Level level, const char* format,
                  va_list args) {
  if (!skyline::log::Enabled(level)) return;
  va_list copy;
  va_copy(copy, args);
  const int length = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (length < 0) return;
  std::vector<char> message(static_cast<size_t>(length) + 1);
  std::vsnprintf(message.data(), message.size(), format, args);
  skyline::log::Write(level, "sharedMemory", "%s", message.data());
}

#define SKYLINE_LOGGER_METHOD(name, level)            \
  void Logger::name(const char* format, ...) const {  \
    va_list args;                                     \
    va_start(args, format);                           \
    Emit(skyline::log::Level::level, format, args);   \
    va_end(args);                                     \
  }

SKYLINE_LOGGER_METHOD(debug, kDebug)
SKYLINE_LOGGER_METHOD(info, kInfo)
SKYLINE_LOGGER_METHOD(warn, kWarning)
SKYLINE_LOGGER_METHOD(error, kError)

#undef SKYLINE_LOGGER_METHOD

void Init() {
  logger = std::make_shared<Logger>();
  if (skyline::config::Flag(skyline::config::kSharedMemoryLog)) {
    skyline::log::LowerThreshold(skyline::log::Level::kDebug);
  }
}

}  // namespace Logger

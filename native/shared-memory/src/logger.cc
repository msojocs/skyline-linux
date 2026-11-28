#include "logger.hh"

#include <cstdio>

#include "skyline/config.h"

namespace Logger {

std::shared_ptr<Logger> logger;

void Logger::Emit(const char* level, const char* message) {
  if (!skyline::config::Flag(skyline::config::kSharedMemoryLog)) return;
  std::fprintf(stderr, "[sharedMemory:%s] %s\n", level, message);
}

void Init() {
  logger = std::make_shared<Logger>();
}

}  // namespace Logger

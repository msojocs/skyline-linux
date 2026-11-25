#include "logger.hh"

#include <cstdio>
#include <cstdlib>

namespace Logger {

std::shared_ptr<Logger> logger;

void Logger::Emit(const char* level, const char* message) {
  const char* setting = std::getenv("SKYLINE_SHARED_MEMORY_LOG");
  if (setting == nullptr || setting[0] == '\0') return;
  std::fprintf(stderr, "[sharedMemory:%s] %s\n", level, message);
}

void Init() {
  logger = std::make_shared<Logger>();
}

}  // namespace Logger

#include "backend/log.h"

#include <atomic>

namespace nr {

namespace {
std::atomic<LogSink> g_sink{nullptr};
}

void SetLogSink(LogSink sink) { g_sink.store(sink); }

void Logv(LogLevel level, const char* format, va_list args) {
  char buffer[2048];
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  if (LogSink sink = g_sink.load()) {
    sink(level, buffer);
  } else {
    std::fprintf(stderr, "%s\n", buffer);
  }
}

}  // namespace nr

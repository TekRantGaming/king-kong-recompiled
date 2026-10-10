// Logging for the SDK-free backend libraries: the plugin installs a sink
// that forwards to the runtime's log; without one (tests) messages go to
// stderr.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace nr {

enum class LogLevel { kDebug, kInfo, kWarning, kError };
using LogSink = void (*)(LogLevel level, const char* text);

void SetLogSink(LogSink sink);
void Logv(LogLevel level, const char* format, va_list args);

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
inline void Logf(LogLevel level, const char* format, ...) {
  va_list args;
  va_start(args, format);
  Logv(level, format, args);
  va_end(args);
}

}  // namespace nr

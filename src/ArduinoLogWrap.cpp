#include <cstdarg>

extern "C" int log_printfv(const char* format, va_list args);

extern "C" int __wrap_log_printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int written = log_printfv(format, args);
  va_end(args);
  return written;
}

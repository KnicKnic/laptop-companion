#include "AppLog.h"

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>

namespace {

bool serialLoggingEnabled = true;

}  // namespace

void setSerialLoggingEnabled(bool enabled) {
  serialLoggingEnabled = enabled;
}

void logPrintf(const char* format, ...) {
  if (!serialLoggingEnabled) return;

  char message[256];

  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  Serial.printf("[%lu ms] %s", static_cast<unsigned long>(millis()), message);
}

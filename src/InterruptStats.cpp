#include "InterruptStats.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t freertosTickCpu0 = 0;
volatile uint32_t freertosTickCpu1 = 0;
}  // namespace

void recordFreeRtosTickFromIsr() {
  const BaseType_t cpu = xPortGetCoreID();
  portENTER_CRITICAL_ISR(&statsMux);
  if (cpu == 0) {
    ++freertosTickCpu0;
  } else {
    ++freertosTickCpu1;
  }
  portEXIT_CRITICAL_ISR(&statsMux);
}

InterruptStatsSnapshot copyInterruptStats() {
  InterruptStatsSnapshot snapshot;
  portENTER_CRITICAL(&statsMux);
  snapshot.freertosTickCpu0 = freertosTickCpu0;
  snapshot.freertosTickCpu1 = freertosTickCpu1;
  portEXIT_CRITICAL(&statsMux);
  return snapshot;
}


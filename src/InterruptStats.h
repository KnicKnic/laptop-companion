#pragma once

#include <stdint.h>

struct InterruptStatsSnapshot {
  uint32_t freertosTickCpu0 = 0;
  uint32_t freertosTickCpu1 = 0;
};

void recordFreeRtosTickFromIsr();
InterruptStatsSnapshot copyInterruptStats();


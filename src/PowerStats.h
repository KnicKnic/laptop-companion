#pragma once

#include <Arduino.h>
#include <esp_err.h>

struct PowerStatsSnapshot {
  bool pmEnabledBySettings = false;
  bool pmConfigured = false;
  bool autoLightSleep = false;
  bool pmProfilingAvailable = false;
  esp_err_t pmConfigResult = ESP_OK;
  int maxFreqMhz = 0;
  int minFreqMhz = 0;
  uint64_t uptimeUs = 0;
  uint64_t lightSleepUs = 0;
  uint64_t lightSleepEntries = 0;
  uint64_t lightSleepRejects = 0;
  uint64_t freq10MhzUs = 0;
  uint64_t freq40MhzUs = 0;
  uint64_t freq80MhzUs = 0;
  uint64_t freq160MhzUs = 0;
  uint64_t freqOtherUs = 0;
};

bool beginPowerStats();
PowerStatsSnapshot copyPowerStats();

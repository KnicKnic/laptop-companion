#pragma once

#include <Arduino.h>
#include <esp_err.h>

#include <cstdint>
#include <string>

constexpr uint8_t POWER_STATS_WAKE_CAUSE_COUNT = 18;
constexpr uint8_t POWER_STATS_WAKE_CAUSE_UNKNOWN_INDEX = POWER_STATS_WAKE_CAUSE_COUNT - 1;
constexpr uint8_t POWER_STATS_REQUEST_BUCKET_COUNT = 6;

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
  uint64_t lightSleepRequestedUs = 0;
  uint64_t lightSleepEntries = 0;
  uint64_t lightSleepRejects = 0;
  uint64_t lightSleepEarlyWakeCount = 0;
  uint64_t wakeCauseCounts[POWER_STATS_WAKE_CAUSE_COUNT] = {};
  uint64_t requestBucketCounts[POWER_STATS_REQUEST_BUCKET_COUNT] = {};
  uint64_t freq10MhzUs = 0;
  uint64_t freq40MhzUs = 0;
  uint64_t freq80MhzUs = 0;
  uint64_t freq160MhzUs = 0;
  uint64_t freqOtherUs = 0;
  uint32_t renderRequests = 0;
};

bool beginPowerStats();
PowerStatsSnapshot copyPowerStats();
std::string formatPowerStatsTotalLine(const PowerStatsSnapshot& power);
std::string formatPowerStatsDeltaLine(const PowerStatsSnapshot& power);
std::string formatPowerStatsAccountingLine(const PowerStatsSnapshot& power);
std::string formatPowerStatsWakeLine(const PowerStatsSnapshot& power);
std::string formatEspTimerActivity();
std::string formatEspTimerAlarmLine();
uint8_t formatTaskActivity(std::string* lines, uint8_t maxLines);
void formatPmLockActivity(std::string& line1, std::string& line2, std::string& line3, std::string& line4,
                          std::string& line5);
void setBtLockTraceConnectionParams(uint16_t intervalUnits, uint16_t latency);
std::string formatBtLockTraceDiagnostics();

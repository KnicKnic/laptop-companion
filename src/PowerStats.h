#pragma once

#include <Arduino.h>
#include <esp_err.h>

#include <cstdint>
#include <string>

constexpr uint8_t POWER_STATS_WAKE_CAUSE_COUNT = 19;
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
  // Monotonic elapsed wall time since power statistics started. Unlike the PM
  // profiling mode totals, this is the ground-truth denominator for interval
  // percentages and is independent of core count.
  uint64_t wallUptimeUs = 0;
  uint64_t uptimeUs = 0;
  uint64_t lightSleepUs = 0;
  uint64_t lightSleepRequestedUs = 0;
  // Number of FreeRTOS idle windows which reached the PM callback.  This is
  // not necessarily a successful esp_light_sleep_start().
  uint64_t lightSleepAttempts = 0;
  // Successful esp_light_sleep_start() calls when PM profiling is available.
  uint64_t lightSleepEntries = 0;
  uint64_t lightSleepRejects = 0;
  uint64_t lightSleepEarlyWakeCount = 0;
  // Indexed by esp_sleep_source_t, with the final slot reserved for values
  // outside the SDK's known wake-source range.
  uint64_t wakeCauseCounts[POWER_STATS_WAKE_CAUSE_COUNT] = {};
  uint64_t wakeCauseSleepUs[POWER_STATS_WAKE_CAUSE_COUNT] = {};
  // One increment per sampled wake bitmap.  This differs from the sum of
  // wakeCauseCounts when an event reports more than one source bit.
  uint64_t wakeCauseSampleCount = 0;
  // OR-mask of every raw source bit observed since boot.
  uint32_t observedWakeCauseBits = 0;
  uint32_t lastWakeCauseBits = 0;
  // OR of bits returned by esp_sleep_get_wakeup_causes() which do not map to
  // a supported ESP-IDF wake source.  Zero means every reported bit decoded.
  uint32_t unmappedWakeCauseBits = 0;
  uint64_t requestBucketCounts[POWER_STATS_REQUEST_BUCKET_COUNT] = {};
  // These are mutually exclusive PM modes, not overlapping individual-lock
  // durations: CPU_MAX wins over APB_MAX, and APB_MIN is the DFS awake floor.
  uint64_t cpuMaxUs = 0;
  uint64_t apbMaxUs = 0;
  uint64_t dfsAwakeUs = 0;
  uint32_t renderRequests = 0;
};

bool beginPowerStats();
PowerStatsSnapshot copyPowerStats();
std::string formatPowerStatsTotalLine(const PowerStatsSnapshot& power);
std::string formatPowerStatsDeltaLine(const PowerStatsSnapshot& power);
std::string formatPowerStatsAccountingLine(const PowerStatsSnapshot& power);
void formatPowerStatsFrequencyLines(const PowerStatsSnapshot& power, std::string& cpuMaxLine, std::string& apbMaxLine,
                                    std::string& dfsLine);
uint8_t formatPowerStatsWakeDeltaLines(const PowerStatsSnapshot& power, std::string* lines, uint8_t maxLines);
std::string formatEspTimerActivity();
std::string formatEspTimerAlarmLine();
uint8_t formatTaskActivity(std::string* lines, uint8_t maxLines);
void formatPmLockActivity(std::string& line1, std::string& line2, std::string& line3, std::string& line4,
                          std::string& line5, std::string& line6, std::string& line7);
void setBtLockTraceConnectionParams(uint16_t intervalUnits, uint16_t latency);
std::string formatBtLockTraceDiagnostics();

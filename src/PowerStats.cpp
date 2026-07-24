#include "PowerStats.h"

#include "AppLog.h"
#include "Settings.h"

#include <esp_pm.h>
#include <esp_timer.h>
#include <esp_private/pm_impl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kMaxCpuMhz = 160;
constexpr int kMinCpuMhz = 10;

int64_t startedAtUs = 0;

PowerStatsSnapshot stats;

void addFreqResidency(PowerStatsSnapshot& snapshot, uint32_t freqMhz, uint64_t elapsedUs) {
  if (freqMhz == 10) {
    snapshot.freq10MhzUs += elapsedUs;
  } else if (freqMhz == 40) {
    snapshot.freq40MhzUs += elapsedUs;
  } else if (freqMhz == 80) {
    snapshot.freq80MhzUs += elapsedUs;
  } else if (freqMhz == 160) {
    snapshot.freq160MhzUs += elapsedUs;
  } else {
    snapshot.freqOtherUs += elapsedUs;
  }
}

void parseModeStatsLine(PowerStatsSnapshot& snapshot, const char* line) {
  char mode[16] = {};
  unsigned long freqMhz = 0;
  unsigned long long timeUs = 0;
  if (std::sscanf(line, " %15s %luM %llu", mode, &freqMhz, &timeUs) != 3) {
    return;
  }
  if (std::strcmp(mode, "SLEEP") == 0) {
    snapshot.lightSleepUs += timeUs;
  } else if (std::strcmp(mode, "APB_MIN") == 0 || std::strcmp(mode, "APB_MAX") == 0 ||
             std::strcmp(mode, "CPU_MAX") == 0) {
    addFreqResidency(snapshot, static_cast<uint32_t>(freqMhz), timeUs);
  }
}

void parseSleepStatsLine(PowerStatsSnapshot& snapshot, const char* line) {
  unsigned long counts = 0;
  unsigned long rejects = 0;
  if (std::sscanf(line, " light_sleep_counts:%lu light_sleep_reject_counts:%lu", &counts, &rejects) == 2) {
    snapshot.lightSleepEntries = counts;
    snapshot.lightSleepRejects = rejects;
  }
}

bool loadPmProfilingStats(PowerStatsSnapshot& snapshot) {
#if CONFIG_PM_PROFILING
  char* dump = nullptr;
  size_t dumpSize = 0;
  FILE* stream = open_memstream(&dump, &dumpSize);
  if (stream == nullptr) {
    return false;
  }

  esp_pm_impl_dump_stats(stream);
  fclose(stream);
  if (dump == nullptr) {
    return false;
  }

  for (char* line = dump; line != nullptr && *line != '\0';) {
    char* next = std::strchr(line, '\n');
    if (next != nullptr) {
      *next = '\0';
      ++next;
    }
    parseModeStatsLine(snapshot, line);
    parseSleepStatsLine(snapshot, line);
    line = next;
  }

  std::free(dump);
  snapshot.uptimeUs = snapshot.lightSleepUs + snapshot.freq10MhzUs + snapshot.freq40MhzUs + snapshot.freq80MhzUs +
                      snapshot.freq160MhzUs + snapshot.freqOtherUs;
  snapshot.pmProfilingAvailable = snapshot.uptimeUs > 0;
  return snapshot.pmProfilingAvailable;
#else
  (void)snapshot;
  return false;
#endif
}

}  // namespace

bool beginPowerStats() {
  startedAtUs = esp_timer_get_time();
  const CompanionSettings settings = copySettings();
  stats.pmEnabledBySettings = settings.system.powerManagement.enable;
  stats.autoLightSleep = settings.system.powerManagement.autoLightSleep;
  stats.maxFreqMhz = kMaxCpuMhz;
  stats.minFreqMhz = kMinCpuMhz;

  if (!settings.system.powerManagement.enable) {
    logPrintf("Power management disabled by settings.\n");
    return true;
  }

  esp_pm_config_t pmConfig = {};
  pmConfig.max_freq_mhz = kMaxCpuMhz;
  pmConfig.min_freq_mhz = kMinCpuMhz;
  pmConfig.light_sleep_enable = settings.system.powerManagement.autoLightSleep;

  const esp_err_t configResult = esp_pm_configure(&pmConfig);

  esp_pm_config_t activeConfig = {};
  const esp_err_t getConfigResult = esp_pm_get_configuration(&activeConfig);

  stats.pmConfigResult = configResult;
  stats.pmConfigured = configResult == ESP_OK;
  if (getConfigResult == ESP_OK) {
    stats.maxFreqMhz = activeConfig.max_freq_mhz;
    stats.minFreqMhz = activeConfig.min_freq_mhz;
    stats.autoLightSleep = activeConfig.light_sleep_enable;
  } else {
    stats.maxFreqMhz = pmConfig.max_freq_mhz;
    stats.minFreqMhz = pmConfig.min_freq_mhz;
    stats.autoLightSleep = pmConfig.light_sleep_enable && configResult == ESP_OK;
  }

  logPrintf("Power management: %s, DFS %d->%d MHz, auto light sleep %s\n",
            configResult == ESP_OK ? "enabled" : esp_err_to_name(configResult), stats.maxFreqMhz, stats.minFreqMhz,
            stats.autoLightSleep ? "on" : "off");
  return configResult == ESP_OK;
}

PowerStatsSnapshot copyPowerStats() {
  PowerStatsSnapshot snapshot = stats;
  if (!loadPmProfilingStats(snapshot)) {
    const int64_t nowUs = esp_timer_get_time();
    snapshot.uptimeUs = nowUs > startedAtUs ? static_cast<uint64_t>(nowUs - startedAtUs) : 0;
  }
  return snapshot;
}

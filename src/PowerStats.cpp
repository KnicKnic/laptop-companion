#include "PowerStats.h"

#include "AppLog.h"
#include "DisplayWorker.h"
#include "PowerManagement.h"

#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_private/pm_impl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
esp_err_t __real_esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg, const char* name,
                                    esp_pm_lock_handle_t* out_handle);
esp_err_t __real_esp_pm_lock_acquire(esp_pm_lock_handle_t handle);
esp_err_t __real_esp_pm_lock_release(esp_pm_lock_handle_t handle);
esp_err_t __real_esp_pm_lock_delete(esp_pm_lock_handle_t handle);
}

namespace {

constexpr uint8_t kTimerTopCount = 5;
constexpr uint8_t kTimerMaxRows = 48;
constexpr size_t kTimerNameLen = 21;
constexpr uint8_t kTaskTopCount = 10;
constexpr uint8_t kTaskMaxRows = 48;
constexpr size_t kTaskNameLen = 17;
constexpr uint8_t kPmLockTopCount = 5;
constexpr uint8_t kPmLockMaxRows = 24;
constexpr size_t kBtLockMaxRows = 8;
constexpr uint64_t kBtLockLongHoldUs = 30ULL * 1000ULL;
constexpr size_t kBtHoldBucketCount = 5;

struct NamedDelta {
  const char* name = "";
  uint64_t count = 0;
};

struct TimerActivitySnapshot {
  char name[kTimerNameLen] = {};
  uint64_t periodUs = 0;
  uint64_t alarmUs = 0;
  uint64_t triggered = 0;
  uint64_t armed = 0;
  uint64_t skipped = 0;
  uint64_t callbackTimeUs = 0;
};

struct TimerActivityDelta {
  char name[kTimerNameLen] = {};
  uint64_t triggered = 0;
  uint64_t armed = 0;
  uint64_t skipped = 0;
  uint64_t callbackTimeUs = 0;
};

struct TimerAlarmRow {
  char name[kTimerNameLen] = {};
  uint64_t dueUs = 0;
  uint64_t periodUs = 0;
  bool valid = false;
};

struct TaskActivitySnapshot {
  char name[kTaskNameLen] = {};
  uint32_t taskNumber = 0;
  uint64_t runtime = 0;
};

struct TaskActivityDelta {
  char name[kTaskNameLen] = {};
  uint64_t runtimeDelta = 0;
  bool valid = false;
};

struct PmLockActivityRow {
  char name[16] = {};
  char type[15] = {};
  int active = 0;
  unsigned long long totalCount = 0;
  unsigned long long timeUs = 0;
  bool valid = false;
};

struct PmLockActivityDelta {
  char name[16] = {};
  char type[15] = {};
  int active = 0;
  unsigned long long countDelta = 0;
  unsigned long long timeDeltaUs = 0;
  bool valid = false;
};

struct BtLockMeta {
  esp_pm_lock_handle_t handle = nullptr;
  int depth = 0;
  int64_t acquiredAtUs = 0;
  uint64_t sleepGapBeforeAcquireUs = 0;
  uint64_t acquirePeriodUs = 0;
  bool used = false;
};

int64_t startedAtUs = 0;
PowerStatsSnapshot stats;
PowerStatsSnapshot previousPowerStats;
bool hasPreviousPowerStats = false;

portMUX_TYPE lightSleepStatsMux = portMUX_INITIALIZER_UNLOCKED;
uint64_t lightSleepAttemptCount = 0;
uint64_t lightSleepTotalUs = 0;
uint64_t lightSleepRequestedTotalUs = 0;
uint64_t lightSleepEarlyWakeCount = 0;
int64_t lightSleepEnteredAtUs = 0;
uint64_t lightSleepRequestedUs = 0;
uint64_t lightSleepWakeCauseCounts[POWER_STATS_WAKE_CAUSE_COUNT] = {};
uint64_t lightSleepWakeCauseUs[POWER_STATS_WAKE_CAUSE_COUNT] = {};
uint64_t lightSleepWakeCauseSampleCount = 0;
uint32_t observedLightSleepWakeCauseBits = 0;
uint32_t lastLightSleepWakeCauseBits = 0;
uint32_t unmappedLightSleepWakeCauseBits = 0;
uint64_t lightSleepRequestBucketCounts[POWER_STATS_REQUEST_BUCKET_COUNT] = {};
bool lightSleepCallbacksRegistered = false;

TimerActivitySnapshot previousTimerActivity[kTimerMaxRows];
uint8_t previousTimerActivityCount = 0;
bool hasPreviousTimerActivity = false;
TaskActivitySnapshot previousTaskActivity[kTaskMaxRows];
uint8_t previousTaskActivityCount = 0;
uint64_t previousTaskTotalRuntime = 0;
bool hasPreviousTaskActivity = false;
PmLockActivityRow previousPmLockActivity[kPmLockMaxRows];
uint8_t previousPmLockActivityCount = 0;
uint64_t previousPmLockActivityTimeUs = 0;
bool hasPreviousPmLockActivity = false;

portMUX_TYPE btLockMux = portMUX_INITIALIZER_UNLOCKED;
BtLockMeta btLocks[kBtLockMaxRows];
uint64_t btLastAcquireAtUs = 0;
uint64_t btLastReleaseAtUs = 0;
uint64_t btIntervalLongHoldCount = 0;
uint64_t btIntervalLongHoldTotalUs = 0;
uint64_t btIntervalLongHoldMaxUs = 0;
uint64_t btIntervalHoldBuckets[kBtHoldBucketCount] = {};
uint64_t btLastLongHoldUs = 0;
uint64_t btLastLongSleepGapUs = 0;
uint64_t btLastLongAcquirePeriodUs = 0;
uint64_t btLastLongReleasePeriodUs = 0;
uint32_t btConnectionIntervalUs = 0;
uint16_t btConnectionLatency = 0;

void formatDurationUsShort(uint64_t durationUs, char* buffer, size_t bufferSize) {
  const uint64_t totalMs = durationUs / 1000ULL;
  if (totalMs < 1000ULL) {
    snprintf(buffer, bufferSize, "%llums", static_cast<unsigned long long>(totalMs));
    return;
  }

  const uint64_t totalSeconds = totalMs / 1000ULL;
  if (totalSeconds < 60ULL) {
    snprintf(buffer, bufferSize, "%llus", static_cast<unsigned long long>(totalSeconds));
    return;
  }

  const uint64_t minutes = totalSeconds / 60ULL;
  const uint64_t seconds = totalSeconds % 60ULL;
  if (minutes < 60ULL) {
    snprintf(buffer, bufferSize, "%llum%02llus", static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
    return;
  }

  const uint64_t hours = minutes / 60ULL;
  const uint64_t remainingMinutes = minutes % 60ULL;
  snprintf(buffer, bufferSize, "%lluh%02llum", static_cast<unsigned long long>(hours),
           static_cast<unsigned long long>(remainingMinutes));
}

uint64_t counterDelta(uint64_t current, uint64_t previous) {
  return current >= previous ? current - previous : 0;
}

uint8_t lightSleepRequestBucketIndex(uint64_t requestedUs) {
  if (requestedUs < 1000ULL) return 0;
  if (requestedUs < 5000ULL) return 1;
  if (requestedUs < 20000ULL) return 2;
  if (requestedUs < 100000ULL) return 3;
  if (requestedUs < 500000ULL) return 4;
  return 5;
}

bool isEarlyLightSleepWake(uint64_t requestedUs, uint64_t sleptUs) {
  if (requestedUs < 1000ULL) return false;
  constexpr uint64_t pmEarlyWakeMarginUs = 100ULL;
  constexpr uint64_t measurementSlackUs = 500ULL;
  const uint64_t expectedUs = requestedUs > pmEarlyWakeMarginUs ? requestedUs - pmEarlyWakeMarginUs : requestedUs;
  return sleptUs + measurementSlackUs < expectedUs;
}

void recordLightSleepWakeCause(uint8_t cause, uint64_t sleptUs) {
  if (cause >= POWER_STATS_WAKE_CAUSE_COUNT) cause = POWER_STATS_WAKE_CAUSE_UNKNOWN_INDEX;
  lightSleepWakeCauseCounts[cause]++;
  lightSleepWakeCauseUs[cause] += sleptUs;
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t onLightSleepEnter(int64_t sleepTimeUs, void*) {
  const int64_t nowUs = esp_timer_get_time();
  const uint64_t requestedUs = sleepTimeUs > 0 ? static_cast<uint64_t>(sleepTimeUs) : 0;
  portENTER_CRITICAL(&lightSleepStatsMux);
  lightSleepEnteredAtUs = nowUs;
  lightSleepRequestedUs = requestedUs;
  lightSleepAttemptCount++;
  portEXIT_CRITICAL(&lightSleepStatsMux);
  return ESP_OK;
}

esp_err_t onLightSleepExit(int64_t sleptUsFromPm, void*) {
  portENTER_CRITICAL(&lightSleepStatsMux);
  if (lightSleepEnteredAtUs > 0) {
    const uint64_t sleptUs = sleptUsFromPm > 0 ? static_cast<uint64_t>(sleptUsFromPm) : 0;
    if (sleptUs == 0) {
      lightSleepEarlyWakeCount++;
    } else {
      lightSleepTotalUs += sleptUs;
      lightSleepRequestedTotalUs += lightSleepRequestedUs;
      if (isEarlyLightSleepWake(lightSleepRequestedUs, sleptUs)) {
        lightSleepEarlyWakeCount++;
      }
      const uint32_t causes = esp_sleep_get_wakeup_causes();
      lastLightSleepWakeCauseBits = causes;
      lightSleepWakeCauseSampleCount++;
      observedLightSleepWakeCauseBits |= causes;
      // esp_sleep_get_wakeup_causes() returns a bitmap.  In particular,
      // UNDEFINED is BIT(ESP_SLEEP_WAKEUP_UNDEFINED) == 0x00000001, not 0.
      // A numeric zero is defensive only; current ESP-IDF returns that same
      // UNDEFINED bit when no hardware wake source is available.
      if (causes == 0) {
        recordLightSleepWakeCause(ESP_SLEEP_WAKEUP_UNDEFINED, sleptUs);
      } else {
        uint32_t knownBits = 0;
        for (uint8_t cause = ESP_SLEEP_WAKEUP_UNDEFINED; cause < POWER_STATS_WAKE_CAUSE_UNKNOWN_INDEX; cause++) {
          // ALL is a control value for disabling sources, never a wake cause.
          if (cause == ESP_SLEEP_WAKEUP_ALL) continue;
          const uint32_t bit = 1UL << cause;
          if ((causes & bit) == 0) continue;
          knownBits |= bit;
          recordLightSleepWakeCause(cause, sleptUs);
        }
        const uint32_t unmappedBits = causes & ~knownBits;
        if (unmappedBits != 0) {
          unmappedLightSleepWakeCauseBits |= unmappedBits;
          recordLightSleepWakeCause(POWER_STATS_WAKE_CAUSE_UNKNOWN_INDEX, sleptUs);
        }
      }
    }
    lightSleepRequestBucketCounts[lightSleepRequestBucketIndex(lightSleepRequestedUs)]++;
  }
  lightSleepEnteredAtUs = 0;
  lightSleepRequestedUs = 0;
  portEXIT_CRITICAL(&lightSleepStatsMux);
  return ESP_OK;
}
#endif

void registerLightSleepCallbacks() {
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  if (lightSleepCallbacksRegistered) return;
  static esp_pm_sleep_cbs_register_config_t cbs = {
      .enter_cb = onLightSleepEnter,
      .exit_cb = onLightSleepExit,
      .enter_cb_user_arg = nullptr,
      .exit_cb_user_arg = nullptr,
  };
  const esp_err_t err = esp_pm_light_sleep_register_cbs(&cbs);
  lightSleepCallbacksRegistered = err == ESP_OK;
  logPrintf("Power stats light-sleep callbacks %s (%d)\n", lightSleepCallbacksRegistered ? "registered" : "failed",
            err);
#endif
}

void copyLightSleepStats(PowerStatsSnapshot& snapshot) {
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL(&lightSleepStatsMux);
  // Keep the callback count as a fallback for builds without PM profiling.
  // loadPmProfilingStats replaces lightSleepEntries with the authoritative
  // successful esp_light_sleep_start() count on this X4 Pro build.
  snapshot.lightSleepAttempts = lightSleepAttemptCount;
  snapshot.lightSleepEntries = lightSleepAttemptCount;
  snapshot.lightSleepUs = lightSleepTotalUs;
  snapshot.lightSleepRequestedUs = lightSleepRequestedTotalUs;
  snapshot.lightSleepEarlyWakeCount = lightSleepEarlyWakeCount;
  snapshot.wakeCauseSampleCount = lightSleepWakeCauseSampleCount;
  snapshot.observedWakeCauseBits = observedLightSleepWakeCauseBits;
  snapshot.lastWakeCauseBits = lastLightSleepWakeCauseBits;
  snapshot.unmappedWakeCauseBits = unmappedLightSleepWakeCauseBits;
  if (lightSleepEnteredAtUs > 0 && nowUs >= lightSleepEnteredAtUs) {
    snapshot.lightSleepUs += static_cast<uint64_t>(nowUs - lightSleepEnteredAtUs);
    snapshot.lightSleepRequestedUs += lightSleepRequestedUs;
  }
  for (uint8_t i = 0; i < POWER_STATS_WAKE_CAUSE_COUNT; i++) {
    snapshot.wakeCauseCounts[i] = lightSleepWakeCauseCounts[i];
    snapshot.wakeCauseSleepUs[i] = lightSleepWakeCauseUs[i];
  }
  for (uint8_t i = 0; i < POWER_STATS_REQUEST_BUCKET_COUNT; i++) {
    snapshot.requestBucketCounts[i] = lightSleepRequestBucketCounts[i];
  }
  portEXIT_CRITICAL(&lightSleepStatsMux);
}

void parseModeStatsLine(PowerStatsSnapshot& snapshot, const char* line) {
  char mode[16] = {};
  unsigned long freqMhz = 0;
  unsigned long long timeUs = 0;
  if (std::sscanf(line, " %15s %luM %llu", mode, &freqMhz, &timeUs) != 3) return;
  if (std::strcmp(mode, "SLEEP") == 0) {
    // PM profiling measures only time that ESP-IDF actually spent asleep.
    snapshot.lightSleepUs = timeUs;
  } else if (std::strcmp(mode, "CPU_MAX") == 0) {
    snapshot.cpuMaxUs = timeUs;
  } else if (std::strcmp(mode, "APB_MAX") == 0) {
    snapshot.apbMaxUs = timeUs;
  } else if (std::strcmp(mode, "APB_MIN") == 0) {
    snapshot.dfsAwakeUs = timeUs;
  }
}

void parseSleepStatsLine(PowerStatsSnapshot& snapshot, const char* line) {
  unsigned long counts = 0;
  unsigned long rejects = 0;
  if (std::sscanf(line, " light_sleep_counts:%lu light_sleep_reject_counts:%lu", &counts, &rejects) == 2) {
    // Unlike the callback count, this is incremented only after a successful
    // esp_light_sleep_start().
    snapshot.lightSleepEntries = counts;
    snapshot.lightSleepRejects = rejects;
  }
}

bool loadPmProfilingStats(PowerStatsSnapshot& snapshot) {
#if CONFIG_PM_PROFILING
  char* dump = nullptr;
  size_t dumpSize = 0;
  FILE* stream = open_memstream(&dump, &dumpSize);
  if (stream == nullptr) return false;

  esp_pm_impl_dump_stats(stream);
  fclose(stream);
  if (dump == nullptr) return false;

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
  snapshot.uptimeUs = snapshot.lightSleepUs + snapshot.cpuMaxUs + snapshot.apbMaxUs + snapshot.dfsAwakeUs;
  snapshot.pmProfilingAvailable = snapshot.uptimeUs > 0;
  return snapshot.pmProfilingAvailable;
#else
  (void)snapshot;
  return false;
#endif
}

std::string trimTimerField(const char* text, size_t length) {
  std::string value(text, length);
  const size_t first = value.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  const size_t last = value.find_last_not_of(' ');
  return value.substr(first, last - first + 1);
}

void shortenName(const char* name, char* buffer, size_t bufferSize) {
  if (bufferSize == 0) return;
  snprintf(buffer, bufferSize, "%s", name && name[0] ? name : "?");
  if (strlen(buffer) > 10) buffer[10] = '\0';
}

bool parseTimerDumpLine(const char* line, TimerActivitySnapshot& timer) {
  const size_t length = strlen(line);
  if (length < 22 || strncmp(line, "Name", 4) == 0 || strncmp(line, "Timer stats", 11) == 0) return false;

  const std::string name = trimTimerField(line, 20);
  if (name.empty()) return false;

  unsigned long long period = 0;
  long long alarm = 0;
  unsigned long long armed = 0;
  unsigned long long triggered = 0;
  unsigned long long skipped = 0;
  unsigned long long callbackTime = 0;
  if (sscanf(line + 20, "%llu %lld %llu %llu %llu %llu", &period, &alarm, &armed, &triggered, &skipped,
             &callbackTime) != 6) {
    return false;
  }

  snprintf(timer.name, sizeof(timer.name), "%s", name.c_str());
  timer.periodUs = period;
  timer.alarmUs = alarm > 0 ? static_cast<uint64_t>(alarm) : 0;
  timer.triggered = triggered;
  timer.armed = armed;
  timer.skipped = skipped;
  timer.callbackTimeUs = callbackTime;
  return true;
}

int findTimerActivity(const TimerActivitySnapshot* timers, uint8_t count, const char* name) {
  for (uint8_t i = 0; i < count; i++) {
    if (strncmp(timers[i].name, name, kTimerNameLen) == 0) return i;
  }
  return -1;
}

const TimerActivitySnapshot* findPreviousTimerActivity(const char* name) {
  for (uint8_t i = 0; i < previousTimerActivityCount; i++) {
    if (strncmp(previousTimerActivity[i].name, name, kTimerNameLen) == 0) return &previousTimerActivity[i];
  }
  return nullptr;
}

void insertTimerDelta(TimerActivityDelta top[kTimerTopCount], const TimerActivitySnapshot& timer,
                      uint64_t triggeredDelta, uint64_t armedDelta, uint64_t skippedDelta,
                      uint64_t callbackTimeDelta) {
  if (triggeredDelta == 0 && armedDelta == 0 && skippedDelta == 0) return;
  for (uint8_t i = 0; i < kTimerTopCount; i++) {
    const uint64_t candidateEvents = triggeredDelta + skippedDelta;
    const uint64_t existingEvents = top[i].triggered + top[i].skipped;
    if (candidateEvents < existingEvents ||
        (candidateEvents == existingEvents && triggeredDelta < top[i].triggered) ||
        (candidateEvents == existingEvents && triggeredDelta == top[i].triggered && armedDelta <= top[i].armed)) {
      continue;
    }
    for (uint8_t j = kTimerTopCount - 1; j > i; j--) top[j] = top[j - 1];
    snprintf(top[i].name, sizeof(top[i].name), "%s", timer.name);
    top[i].triggered = triggeredDelta;
    top[i].armed = armedDelta;
    top[i].skipped = skippedDelta;
    top[i].callbackTimeUs = callbackTimeDelta;
    return;
  }
}

void insertTimerAlarm(TimerAlarmRow top[kTimerTopCount], const TimerActivitySnapshot& timer, uint64_t nowUs) {
  if (timer.alarmUs == 0) return;
  TimerAlarmRow candidate;
  snprintf(candidate.name, sizeof(candidate.name), "%s", timer.name);
  candidate.dueUs = timer.alarmUs > nowUs ? timer.alarmUs - nowUs : 0;
  candidate.periodUs = timer.periodUs;
  candidate.valid = true;

  for (uint8_t i = 0; i < kTimerTopCount; i++) {
    if (top[i].valid && candidate.dueUs > top[i].dueUs) continue;
    if (top[i].valid && candidate.dueUs == top[i].dueUs && strcmp(candidate.name, top[i].name) >= 0) continue;
    for (uint8_t j = kTimerTopCount - 1; j > i; j--) top[j] = top[j - 1];
    top[i] = candidate;
    return;
  }
}

uint8_t captureTimerActivity(TimerActivitySnapshot* timers, uint8_t maxTimers) {
  char* dump = nullptr;
  size_t dumpSize = 0;
  FILE* stream = open_memstream(&dump, &dumpSize);
  if (!stream) return 0;

  const esp_err_t err = esp_timer_dump(stream);
  fclose(stream);
  if (err != ESP_OK || !dump) {
    free(dump);
    return 0;
  }

  uint8_t count = 0;
  char* cursor = dump;
  while (cursor && *cursor) {
    char* next = strchr(cursor, '\n');
    if (next) *next = '\0';

    TimerActivitySnapshot parsed;
    if (parseTimerDumpLine(cursor, parsed)) {
      const int existing = findTimerActivity(timers, count, parsed.name);
      if (existing >= 0) {
        if (timers[existing].alarmUs == 0 || (parsed.alarmUs != 0 && parsed.alarmUs < timers[existing].alarmUs)) {
          timers[existing].alarmUs = parsed.alarmUs;
        }
        if (timers[existing].periodUs == 0) timers[existing].periodUs = parsed.periodUs;
        timers[existing].triggered += parsed.triggered;
        timers[existing].armed += parsed.armed;
        timers[existing].skipped += parsed.skipped;
        timers[existing].callbackTimeUs += parsed.callbackTimeUs;
      } else if (count < maxTimers) {
        timers[count++] = parsed;
      }
    }

    cursor = next ? next + 1 : nullptr;
  }

  free(dump);
  return count;
}

void storeTimerActivitySnapshot(const TimerActivitySnapshot* timers, uint8_t count) {
  previousTimerActivityCount = std::min(count, static_cast<uint8_t>(kTimerMaxRows));
  for (uint8_t i = 0; i < previousTimerActivityCount; i++) previousTimerActivity[i] = timers[i];
  hasPreviousTimerActivity = true;
}

const TaskActivitySnapshot* findPreviousTaskActivity(uint32_t taskNumber, const char* name) {
  for (uint8_t i = 0; i < previousTaskActivityCount; i++) {
    if (previousTaskActivity[i].taskNumber == taskNumber) return &previousTaskActivity[i];
  }
  for (uint8_t i = 0; i < previousTaskActivityCount; i++) {
    if (strncmp(previousTaskActivity[i].name, name, kTaskNameLen) == 0) return &previousTaskActivity[i];
  }
  return nullptr;
}

void storeTaskActivitySnapshot(const TaskStatus_t* tasks, uint8_t count, uint64_t totalRuntime) {
  previousTaskActivityCount = std::min(count, static_cast<uint8_t>(kTaskMaxRows));
  for (uint8_t i = 0; i < previousTaskActivityCount; i++) {
    snprintf(previousTaskActivity[i].name, sizeof(previousTaskActivity[i].name), "%s",
             tasks[i].pcTaskName ? tasks[i].pcTaskName : "?");
    previousTaskActivity[i].taskNumber = tasks[i].xTaskNumber;
    previousTaskActivity[i].runtime = tasks[i].ulRunTimeCounter;
  }
  previousTaskTotalRuntime = totalRuntime;
  hasPreviousTaskActivity = true;
}

bool taskDeltaRanksAbove(const TaskActivityDelta& candidate, const TaskActivityDelta& existing) {
  if (!existing.valid) return true;
  if (candidate.runtimeDelta != existing.runtimeDelta) return candidate.runtimeDelta > existing.runtimeDelta;
  return strcmp(candidate.name, existing.name) < 0;
}

void insertTaskDelta(TaskActivityDelta top[kTaskTopCount], const TaskStatus_t& task, uint64_t runtimeDelta) {
  if (runtimeDelta == 0) return;
  TaskActivityDelta candidate;
  snprintf(candidate.name, sizeof(candidate.name), "%s", task.pcTaskName ? task.pcTaskName : "?");
  candidate.runtimeDelta = runtimeDelta;
  candidate.valid = true;
  for (uint8_t i = 0; i < kTaskTopCount; i++) {
    if (!taskDeltaRanksAbove(candidate, top[i])) continue;
    for (uint8_t j = kTaskTopCount - 1; j > i; j--) top[j] = top[j - 1];
    top[i] = candidate;
    return;
  }
}

bool capturePmLockDump(char** dump, size_t* dumpSize) {
  *dump = nullptr;
  *dumpSize = 0;
  FILE* stream = open_memstream(dump, dumpSize);
  if (!stream) return false;

  const esp_err_t err = esp_pm_dump_locks(stream);
  fclose(stream);
  if (err != ESP_OK || !*dump) {
    free(*dump);
    *dump = nullptr;
    *dumpSize = 0;
    return false;
  }
  return true;
}

const char* shortPmLockType(const char* type) {
  if (strcmp(type, "NO_LIGHT_SLEEP") == 0) return "noLS";
  if (strcmp(type, "APB_FREQ_MAX") == 0) return "apb";
  if (strcmp(type, "CPU_FREQ_MAX") == 0) return "cpu";
  return type;
}

bool parsePmLockDumpLine(const char* line, PmLockActivityRow& row) {
  char name[16] = {};
  char type[15] = {};
  int arg = 0;
  int active = 0;
  unsigned long long totalCount = 0;
  unsigned long long timeUs = 0;
  unsigned long long timePercent = 0;
  if (sscanf(line, "%15s %14s %d %d %llu %llu %llu%%", name, type, &arg, &active, &totalCount, &timeUs,
             &timePercent) != 7) {
    return false;
  }
  snprintf(row.name, sizeof(row.name), "%s", name);
  snprintf(row.type, sizeof(row.type), "%s", shortPmLockType(type));
  row.active = active;
  row.totalCount = totalCount;
  row.timeUs = timeUs;
  row.valid = true;
  return true;
}

bool parsePmLockDumpTimeLine(const char* line, uint64_t& bootTimeUs) {
  unsigned long long parsed = 0;
  if (sscanf(line, "Time since bootup: %llu us", &parsed) != 1) return false;
  bootTimeUs = parsed;
  return true;
}

const PmLockActivityRow* findPreviousPmLockActivity(const char* name, const char* type) {
  for (uint8_t i = 0; i < previousPmLockActivityCount; i++) {
    if (strncmp(previousPmLockActivity[i].name, name, sizeof(previousPmLockActivity[i].name)) == 0 &&
        strncmp(previousPmLockActivity[i].type, type, sizeof(previousPmLockActivity[i].type)) == 0) {
      return &previousPmLockActivity[i];
    }
  }
  return nullptr;
}

void storePmLockActivitySnapshot(const PmLockActivityRow rows[kPmLockMaxRows], uint8_t rowCount, uint64_t bootTimeUs) {
  previousPmLockActivityCount = rowCount;
  for (uint8_t i = 0; i < rowCount; i++) previousPmLockActivity[i] = rows[i];
  previousPmLockActivityTimeUs = bootTimeUs;
  hasPreviousPmLockActivity = true;
}

bool pmLockDeltaRanksAbove(const PmLockActivityDelta& candidate, const PmLockActivityDelta& existing) {
  if (!existing.valid) return true;
  if (candidate.timeDeltaUs != existing.timeDeltaUs) return candidate.timeDeltaUs > existing.timeDeltaUs;
  if (candidate.countDelta != existing.countDelta) return candidate.countDelta > existing.countDelta;
  if (candidate.active != existing.active) return candidate.active > existing.active;
  return strcmp(candidate.name, existing.name) < 0;
}

void insertPmLockDelta(PmLockActivityDelta top[kPmLockTopCount], const PmLockActivityDelta& row) {
  if (row.timeDeltaUs == 0 && row.countDelta == 0 && row.active == 0) return;
  for (uint8_t i = 0; i < kPmLockTopCount; i++) {
    if (!pmLockDeltaRanksAbove(row, top[i])) continue;
    for (uint8_t j = kPmLockTopCount - 1; j > i; j--) top[j] = top[j - 1];
    top[i] = row;
    return;
  }
}

bool isBtApbLock(esp_pm_lock_type_t lockType, const char* name) {
  return lockType == ESP_PM_APB_FREQ_MAX && name && strcmp(name, "bt") == 0;
}

BtLockMeta* findBtLock(esp_pm_lock_handle_t handle) {
  for (auto& lock : btLocks) {
    if (lock.used && lock.handle == handle) return &lock;
  }
  return nullptr;
}

void trackBtLock(esp_pm_lock_handle_t handle) {
  if (!handle) return;
  portENTER_CRITICAL(&btLockMux);
  if (findBtLock(handle)) {
    portEXIT_CRITICAL(&btLockMux);
    return;
  }
  for (auto& lock : btLocks) {
    if (!lock.used) {
      lock.handle = handle;
      lock.used = true;
      break;
    }
  }
  portEXIT_CRITICAL(&btLockMux);
}

void untrackBtLock(esp_pm_lock_handle_t handle) {
  portENTER_CRITICAL(&btLockMux);
  BtLockMeta* lock = findBtLock(handle);
  if (lock) *lock = BtLockMeta{};
  portEXIT_CRITICAL(&btLockMux);
}

uint8_t btHoldBucketIndex(uint64_t heldUs) {
  if (heldUs < 50ULL * 1000ULL) return 0;
  if (heldUs < 100ULL * 1000ULL) return 1;
  if (heldUs < 200ULL * 1000ULL) return 2;
  if (heldUs < 500ULL * 1000ULL) return 3;
  return 4;
}

uint16_t estimateConnectionEvents(uint64_t periodUs, uint32_t intervalUs) {
  if (periodUs == 0 || intervalUs == 0) return 0;
  const uint64_t roundedEvents = (periodUs + (intervalUs / 2ULL)) / intervalUs;
  return roundedEvents > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(roundedEvents);
}

void recordBtLockAcquire(esp_pm_lock_handle_t handle, esp_err_t err) {
  if (err != ESP_OK) return;
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL(&btLockMux);
  BtLockMeta* lock = findBtLock(handle);
  if (!lock) {
    portEXIT_CRITICAL(&btLockMux);
    return;
  }
  if (lock->depth == 0) {
    lock->acquiredAtUs = nowUs;
    lock->sleepGapBeforeAcquireUs =
        btLastReleaseAtUs > 0 && nowUs >= static_cast<int64_t>(btLastReleaseAtUs)
            ? static_cast<uint64_t>(nowUs) - btLastReleaseAtUs
            : 0;
    lock->acquirePeriodUs =
        btLastAcquireAtUs > 0 && nowUs >= static_cast<int64_t>(btLastAcquireAtUs)
            ? static_cast<uint64_t>(nowUs) - btLastAcquireAtUs
            : 0;
    btLastAcquireAtUs = static_cast<uint64_t>(nowUs);
  }
  lock->depth++;
  portEXIT_CRITICAL(&btLockMux);
}

void recordBtLockRelease(esp_pm_lock_handle_t handle, esp_err_t err) {
  if (err != ESP_OK) return;
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL(&btLockMux);
  BtLockMeta* lock = findBtLock(handle);
  if (!lock || lock->depth <= 0) {
    portEXIT_CRITICAL(&btLockMux);
    return;
  }

  uint64_t heldUs = 0;
  if (lock->acquiredAtUs > 0 && nowUs >= lock->acquiredAtUs) {
    heldUs = static_cast<uint64_t>(nowUs - lock->acquiredAtUs);
  }
  const uint64_t releasePeriodUs =
      btLastReleaseAtUs > 0 && nowUs >= static_cast<int64_t>(btLastReleaseAtUs)
          ? static_cast<uint64_t>(nowUs) - btLastReleaseAtUs
          : 0;
  lock->depth--;
  if (lock->depth == 0) {
    lock->acquiredAtUs = 0;
    btLastReleaseAtUs = nowUs > 0 ? static_cast<uint64_t>(nowUs) : 0;
    if (heldUs > kBtLockLongHoldUs) {
      btIntervalLongHoldCount++;
      btIntervalLongHoldTotalUs += heldUs;
      btIntervalLongHoldMaxUs = std::max(btIntervalLongHoldMaxUs, heldUs);
      btIntervalHoldBuckets[btHoldBucketIndex(heldUs)]++;
      btLastLongHoldUs = heldUs;
      btLastLongSleepGapUs = lock->sleepGapBeforeAcquireUs;
      btLastLongAcquirePeriodUs = lock->acquirePeriodUs;
      btLastLongReleasePeriodUs = releasePeriodUs;
    }
  }
  portEXIT_CRITICAL(&btLockMux);
}

const char* wakeCauseName(uint8_t causeIndex) {
  switch (causeIndex) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "undefined";
    case ESP_SLEEP_WAKEUP_ALL:
      return "all";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "touch";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
      return "uart0";
    case ESP_SLEEP_WAKEUP_UART1:
      return "uart1";
    case ESP_SLEEP_WAKEUP_UART2:
      return "uart2";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "wifi";
    case ESP_SLEEP_WAKEUP_COCPU:
      return "cocpu";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
      return "cocpu_trap";
    case ESP_SLEEP_WAKEUP_BT:
      return "bt";
    case ESP_SLEEP_WAKEUP_VAD:
      return "vad";
    case ESP_SLEEP_WAKEUP_VBAT_UNDER_VOLT:
      return "vbat";
    case POWER_STATS_WAKE_CAUSE_UNKNOWN_INDEX:
      return "unknown";
    default:
      return "invalid";
  }
}

const char* requestBucketName(uint8_t bucketIndex) {
  switch (bucketIndex) {
    case 0:
      return "<1ms";
    case 1:
      return "1-5ms";
    case 2:
      return "5-20ms";
    case 3:
      return "20-100ms";
    case 4:
      return "100-500ms";
    case 5:
      return "500ms+";
    default:
      return "invalid";
  }
}

}  // namespace

bool beginPowerStats() {
  startedAtUs = esp_timer_get_time();
  registerLightSleepCallbacks();
  return true;
}

PowerStatsSnapshot copyPowerStats() {
  PowerStatsSnapshot snapshot = stats;
  const PowerManagementState pm = copyPowerManagementState();
  snapshot.pmEnabledBySettings = pm.enabledBySettings;
  snapshot.pmConfigured = pm.configured;
  snapshot.autoLightSleep = pm.autoLightSleep;
  snapshot.pmConfigResult = pm.configResult;
  snapshot.maxFreqMhz = pm.maxFreqMhz;
  snapshot.minFreqMhz = pm.minFreqMhz;
  snapshot.renderRequests = renderRequestCount();
  copyLightSleepStats(snapshot);
  if (!loadPmProfilingStats(snapshot)) {
    const int64_t nowUs = esp_timer_get_time();
    snapshot.uptimeUs = nowUs > startedAtUs ? static_cast<uint64_t>(nowUs - startedAtUs) : 0;
  }
  return snapshot;
}

std::string formatPowerStatsTotalLine(const PowerStatsSnapshot& power) {
  char slept[16];
  char uptime[16];
  formatDurationUsShort(power.lightSleepUs, slept, sizeof(slept));
  formatDurationUsShort(power.uptimeUs, uptime, sizeof(uptime));
  const uint64_t percentTenths = power.uptimeUs > 0 ? (power.lightSleepUs * 1000ULL) / power.uptimeUs : 0;

  char line[112];
  snprintf(line, sizeof(line), "Sleep: %llu ok / %llu candidates, %s / %s (%llu.%01llu%%)",
           static_cast<unsigned long long>(power.lightSleepEntries),
           static_cast<unsigned long long>(power.lightSleepAttempts), slept, uptime,
           static_cast<unsigned long long>(percentTenths / 10ULL),
           static_cast<unsigned long long>(percentTenths % 10ULL));
  return line;
}

std::string formatPowerStatsDeltaLine(const PowerStatsSnapshot& power) {
  const uint64_t enterDelta = hasPreviousPowerStats ? counterDelta(power.lightSleepEntries,
                                                                   previousPowerStats.lightSleepEntries)
                                                    : 0;
  const uint64_t attemptDelta = hasPreviousPowerStats ? counterDelta(power.lightSleepAttempts,
                                                                      previousPowerStats.lightSleepAttempts)
                                                      : 0;
  const uint64_t sleptDelta = hasPreviousPowerStats ? counterDelta(power.lightSleepUs, previousPowerStats.lightSleepUs)
                                                    : 0;
  const uint64_t elapsedDelta = hasPreviousPowerStats ? counterDelta(power.uptimeUs, previousPowerStats.uptimeUs) : 0;
  const uint64_t percentTenths = elapsedDelta > 0 ? (sleptDelta * 1000ULL) / elapsedDelta : 0;

  char slept[16];
  char elapsed[16];
  formatDurationUsShort(sleptDelta, slept, sizeof(slept));
  formatDurationUsShort(elapsedDelta, elapsed, sizeof(elapsed));

  char line[112];
  snprintf(line, sizeof(line), "Sleep delta: +%llu ok / +%llu candidates, %s / %s (%llu.%01llu%%)",
           static_cast<unsigned long long>(enterDelta), static_cast<unsigned long long>(attemptDelta), slept, elapsed,
           static_cast<unsigned long long>(percentTenths / 10ULL),
           static_cast<unsigned long long>(percentTenths % 10ULL));
  return line;
}

std::string formatPowerStatsAccountingLine(const PowerStatsSnapshot& power) {
  const uint64_t sleptDelta = hasPreviousPowerStats ? counterDelta(power.lightSleepUs, previousPowerStats.lightSleepUs)
                                                    : 0;
  const uint64_t requestedDelta =
      hasPreviousPowerStats ? counterDelta(power.lightSleepRequestedUs, previousPowerStats.lightSleepRequestedUs) : 0;
  const uint64_t requestedNotSleptDelta = requestedDelta > sleptDelta ? requestedDelta - sleptDelta : 0;

  char requested[16];
  char actual[16];
  char lost[16];
  formatDurationUsShort(requestedDelta, requested, sizeof(requested));
  formatDurationUsShort(sleptDelta, actual, sizeof(actual));
  formatDurationUsShort(requestedNotSleptDelta, lost, sizeof(lost));

  char line[128];
  snprintf(line, sizeof(line), "Sleep windows: requested %s actual %s miss %s", requested, actual, lost);
  return line;
}

uint8_t formatPowerStatsWakeDeltaLines(const PowerStatsSnapshot& power, std::string* lines, uint8_t maxLines) {
  if (lines == nullptr || maxLines == 0) return 0;
  const bool baseline = !hasPreviousPowerStats;
  const uint8_t count = std::min<uint8_t>(POWER_STATS_WAKE_CAUSE_COUNT, maxLines);
  for (uint8_t i = 0; i < count; ++i) {
    const uint64_t hits = baseline ? 0 : counterDelta(power.wakeCauseCounts[i], previousPowerStats.wakeCauseCounts[i]);
    const uint64_t sleptUs = baseline ? 0 : counterDelta(power.wakeCauseSleepUs[i], previousPowerStats.wakeCauseSleepUs[i]);
    char deltaSlept[16];
    char totalSlept[16];
    formatDurationUsShort(sleptUs, deltaSlept, sizeof(deltaSlept));
    formatDurationUsShort(power.wakeCauseSleepUs[i], totalSlept, sizeof(totalSlept));
    char line[144];
    // The display can redraw twice in quick succession, so retain the
    // lifetime total beside the render-to-render delta.  The latter may be
    // zero even though the cause accumulator is working normally.
    snprintf(line, sizeof(line), "Wake %-10s hit +%llu / %llu sleep +%s / %s%s", wakeCauseName(i),
             static_cast<unsigned long long>(hits), static_cast<unsigned long long>(power.wakeCauseCounts[i]),
             deltaSlept, totalSlept, baseline ? " (baseline)" : "");
    lines[i] = line;
  }
  previousPowerStats = power;
  hasPreviousPowerStats = true;
  return count;
}

std::string formatEspTimerActivity() {
#if CONFIG_ESP_TIMER_PROFILING
  static TimerActivitySnapshot current[kTimerMaxRows];
  static TimerActivityDelta top[kTimerTopCount];
  memset(current, 0, sizeof(current));
  memset(top, 0, sizeof(top));
  const uint8_t currentCount = captureTimerActivity(current, kTimerMaxRows);
  if (currentCount == 0) return "Timers: unavailable";

  if (!hasPreviousTimerActivity) {
    storeTimerActivitySnapshot(current, currentCount);
    return "Timers: baseline";
  }

  for (uint8_t i = 0; i < currentCount; i++) {
    const TimerActivitySnapshot* previous = findPreviousTimerActivity(current[i].name);
    insertTimerDelta(top, current[i], previous ? counterDelta(current[i].triggered, previous->triggered)
                                               : current[i].triggered,
                     previous ? counterDelta(current[i].armed, previous->armed) : current[i].armed,
                     previous ? counterDelta(current[i].skipped, previous->skipped) : current[i].skipped,
                     previous ? counterDelta(current[i].callbackTimeUs, previous->callbackTimeUs)
                              : current[i].callbackTimeUs);
  }
  storeTimerActivitySnapshot(current, currentCount);

  std::string line = "Timers:";
  if (top[0].triggered == 0 && top[0].armed == 0 && top[0].skipped == 0) return line + " none";

  for (uint8_t i = 0; i < kTimerTopCount; i++) {
    if (top[i].triggered == 0 && top[i].armed == 0 && top[i].skipped == 0) break;
    char name[12];
    shortenName(top[i].name, name, sizeof(name));
    char part[32];
    if (top[i].triggered > 0 && top[i].skipped > 0) {
      snprintf(part, sizeof(part), " %s+%llu/s%llu", name, static_cast<unsigned long long>(top[i].triggered),
               static_cast<unsigned long long>(top[i].skipped));
    } else if (top[i].triggered > 0) {
      snprintf(part, sizeof(part), " %s+%llu", name, static_cast<unsigned long long>(top[i].triggered));
    } else if (top[i].skipped > 0) {
      snprintf(part, sizeof(part), " %s skip+%llu", name, static_cast<unsigned long long>(top[i].skipped));
    } else {
      snprintf(part, sizeof(part), " %s armed+%llu", name, static_cast<unsigned long long>(top[i].armed));
    }
    line += part;
  }
  return line;
#else
  return "Timers: profiling off";
#endif
}

std::string formatEspTimerAlarmLine() {
#if CONFIG_ESP_TIMER_PROFILING
  static TimerActivitySnapshot current[kTimerMaxRows];
  static TimerAlarmRow top[kTimerTopCount];
  memset(current, 0, sizeof(current));
  memset(top, 0, sizeof(top));
  const uint8_t currentCount = captureTimerActivity(current, kTimerMaxRows);
  if (currentCount == 0) return "Alarm: unavailable";

  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  for (uint8_t i = 0; i < currentCount; i++) {
    insertTimerAlarm(top, current[i], nowUs);
  }

  if (!top[0].valid) return "Alarm: none";

  char nextWake[16];
  const int64_t nextWakeAt = esp_timer_get_next_alarm_for_wake_up();
  if (nextWakeAt > static_cast<int64_t>(nowUs)) {
    formatDurationUsShort(static_cast<uint64_t>(nextWakeAt) - nowUs, nextWake, sizeof(nextWake));
  } else {
    snprintf(nextWake, sizeof(nextWake), "now");
  }

  std::string line = "Alarm:";
  char prefix[24];
  snprintf(prefix, sizeof(prefix), " wake%s", nextWake);
  line += prefix;

  for (uint8_t i = 0; i < kTimerTopCount; i++) {
    if (!top[i].valid) break;
    char name[12];
    char due[16];
    shortenName(top[i].name, name, sizeof(name));
    formatDurationUsShort(top[i].dueUs, due, sizeof(due));

    char part[36];
    if (top[i].periodUs > 0) {
      char period[16];
      formatDurationUsShort(top[i].periodUs, period, sizeof(period));
      snprintf(part, sizeof(part), " %s@%s/%s", name, due, period);
    } else {
      snprintf(part, sizeof(part), " %s@%s", name, due);
    }
    line += part;
  }
  return line;
#else
  return "Alarm: profiling off";
#endif
}

uint8_t formatTaskActivity(std::string* lines, uint8_t maxLines) {
  if (!lines || maxLines == 0) return 0;

#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
  static TaskStatus_t tasks[kTaskMaxRows];
  static TaskActivityDelta top[kTaskTopCount];
  memset(tasks, 0, sizeof(tasks));
  memset(top, 0, sizeof(top));
  configRUN_TIME_COUNTER_TYPE totalRuntimeRaw = 0;
  const UBaseType_t captured = uxTaskGetSystemState(tasks, kTaskMaxRows, &totalRuntimeRaw);
  const uint8_t taskCount = static_cast<uint8_t>(std::min<UBaseType_t>(captured, kTaskMaxRows));
  const uint64_t totalRuntime = static_cast<uint64_t>(totalRuntimeRaw);
  if (taskCount == 0) {
    lines[0] = "Task01: unavailable";
    return 1;
  }

  if (!hasPreviousTaskActivity || totalRuntime <= previousTaskTotalRuntime) {
    storeTaskActivitySnapshot(tasks, taskCount, totalRuntime);
    lines[0] = "Task01: baseline";
    return 1;
  }

  uint64_t summedTaskDelta = 0;
  for (uint8_t i = 0; i < taskCount; i++) {
    const TaskActivitySnapshot* previous = findPreviousTaskActivity(tasks[i].xTaskNumber, tasks[i].pcTaskName);
    const uint64_t currentRuntime = static_cast<uint64_t>(tasks[i].ulRunTimeCounter);
    const uint64_t runtimeDelta = previous ? counterDelta(currentRuntime, previous->runtime) : currentRuntime;
    summedTaskDelta += runtimeDelta;
    insertTaskDelta(top, tasks[i], runtimeDelta);
  }

  const uint64_t totalDelta = counterDelta(totalRuntime, previousTaskTotalRuntime);
  const uint64_t denominator = totalDelta > 0 ? totalDelta : summedTaskDelta;
  storeTaskActivitySnapshot(tasks, taskCount, totalRuntime);

  uint8_t emitted = 0;
  const uint8_t limit = std::min<uint8_t>(std::min<uint8_t>(maxLines, kTaskTopCount), taskCount);
  for (uint8_t i = 0; i < limit; i++) {
    if (!top[i].valid) break;

    char name[12];
    shortenName(top[i].name, name, sizeof(name));
    char runtime[16];
    formatDurationUsShort(top[i].runtimeDelta, runtime, sizeof(runtime));
    const uint64_t percentTenths = denominator > 0 ? (top[i].runtimeDelta * 1000ULL) / denominator : 0;

    char line[96];
    snprintf(line, sizeof(line), "Task%02u: %s %llu.%01llu%% %s", static_cast<unsigned>(i + 1), name,
             static_cast<unsigned long long>(percentTenths / 10ULL),
             static_cast<unsigned long long>(percentTenths % 10ULL), runtime);
    lines[emitted++] = line;
  }

  if (emitted == 0) {
    lines[0] = "Task01: none";
    return 1;
  }
  return emitted;
#else
  lines[0] = "Task01: profiling off";
  return 1;
#endif
}

void formatPmLockActivity(std::string& line1, std::string& line2, std::string& line3, std::string& line4,
                          std::string& line5) {
#if CONFIG_PM_PROFILING
  char* dump = nullptr;
  size_t dumpSize = 0;
  if (!capturePmLockDump(&dump, &dumpSize)) {
    line1 = "PM1: unavailable";
    line2 = "PM2: unavailable";
    line3 = "PM3: unavailable";
    line4 = "PM4: unavailable";
    line5 = "PM5: unavailable";
    return;
  }

  static PmLockActivityRow currentRows[kPmLockMaxRows];
  memset(currentRows, 0, sizeof(currentRows));
  uint8_t currentRowCount = 0;
  uint64_t bootTimeUs = 0;
  char* cursor = dump;
  while (cursor && *cursor) {
    char* next = strchr(cursor, '\n');
    if (next) *next = '\0';

    parsePmLockDumpTimeLine(cursor, bootTimeUs);
    PmLockActivityRow row;
    if (currentRowCount < kPmLockMaxRows && parsePmLockDumpLine(cursor, row)) {
      currentRows[currentRowCount++] = row;
    }
    cursor = next ? next + 1 : nullptr;
  }
  free(dump);

  if (bootTimeUs == 0) bootTimeUs = static_cast<uint64_t>(esp_timer_get_time());
  if (!hasPreviousPmLockActivity || bootTimeUs <= previousPmLockActivityTimeUs) {
    storePmLockActivitySnapshot(currentRows, currentRowCount, bootTimeUs);
    line1 = "PM1: baseline";
    line2 = "PM2: delta next sample";
    line3 = "PM3: none";
    line4 = "PM4: none";
    line5 = "PM5: none";
    return;
  }

  const uint64_t elapsedUs = bootTimeUs - previousPmLockActivityTimeUs;
  PmLockActivityDelta top[kPmLockTopCount] = {};
  for (uint8_t i = 0; i < currentRowCount; i++) {
    const PmLockActivityRow& current = currentRows[i];
    const PmLockActivityRow* previous = findPreviousPmLockActivity(current.name, current.type);
    if (!previous) continue;

    PmLockActivityDelta delta;
    snprintf(delta.name, sizeof(delta.name), "%s", current.name);
    snprintf(delta.type, sizeof(delta.type), "%s", current.type);
    delta.active = current.active;
    delta.countDelta = counterDelta(current.totalCount, previous->totalCount);
    delta.timeDeltaUs = counterDelta(current.timeUs, previous->timeUs);
    delta.valid = true;
    insertPmLockDelta(top, delta);
  }
  storePmLockActivitySnapshot(currentRows, currentRowCount, bootTimeUs);

  auto formatRow = [elapsedUs](const char* prefix, const PmLockActivityDelta& row) {
    if (!row.valid) return std::string(prefix) + " none";
    char held[16];
    char average[16];
    formatDurationUsShort(row.timeDeltaUs, held, sizeof(held));
    formatDurationUsShort(row.countDelta > 0 ? row.timeDeltaUs / row.countDelta : 0, average, sizeof(average));
    const unsigned long long pctX10 =
        elapsedUs > 0 ? (static_cast<unsigned long long>(row.timeDeltaUs) * 1000ULL) / elapsedUs : 0ULL;

    char line[112];
    snprintf(line, sizeof(line), "%s %s %s +%llu %s avg%s %llu.%llu%% act=%d", prefix, row.name, row.type,
             row.countDelta, held, average, pctX10 / 10ULL, pctX10 % 10ULL, row.active);
    return std::string(line);
  };

  line1 = formatRow("PM1:", top[0]);
  line2 = formatRow("PM2:", top[1]);
  line3 = formatRow("PM3:", top[2]);
  line4 = formatRow("PM4:", top[3]);
  line5 = formatRow("PM5:", top[4]);
#else
  line1 = "PM1: profiling off";
  line2 = "PM2: profiling off";
  line3 = "PM3: profiling off";
  line4 = "PM4: profiling off";
  line5 = "PM5: profiling off";
#endif
}

void setBtLockTraceConnectionParams(uint16_t intervalUnits, uint16_t latency) {
  const uint32_t intervalUs = static_cast<uint32_t>(intervalUnits) * 1250UL;
  portENTER_CRITICAL(&btLockMux);
  btConnectionIntervalUs = intervalUs;
  btConnectionLatency = latency;
  portEXIT_CRITICAL(&btLockMux);
}

std::string formatBtLockTraceDiagnostics() {
  uint64_t count = 0;
  uint64_t totalUs = 0;
  uint64_t maxUs = 0;
  uint64_t buckets[kBtHoldBucketCount] = {};
  uint64_t lastUs = 0;
  uint64_t sleepGapUs = 0;
  uint64_t acquirePeriodUs = 0;
  uint64_t releasePeriodUs = 0;
  uint32_t intervalUs = 0;
  uint16_t latency = 0;

  portENTER_CRITICAL(&btLockMux);
  count = btIntervalLongHoldCount;
  totalUs = btIntervalLongHoldTotalUs;
  maxUs = btIntervalLongHoldMaxUs;
  for (size_t i = 0; i < kBtHoldBucketCount; i++) {
    buckets[i] = btIntervalHoldBuckets[i];
    btIntervalHoldBuckets[i] = 0;
  }
  btIntervalLongHoldCount = 0;
  btIntervalLongHoldTotalUs = 0;
  btIntervalLongHoldMaxUs = 0;
  lastUs = btLastLongHoldUs;
  sleepGapUs = btLastLongSleepGapUs;
  acquirePeriodUs = btLastLongAcquirePeriodUs;
  releasePeriodUs = btLastLongReleasePeriodUs;
  intervalUs = btConnectionIntervalUs;
  latency = btConnectionLatency;
  portEXIT_CRITICAL(&btLockMux);

  if (count == 0) return "BTLD: no >30ms bt holds";

  char total[16];
  char average[16];
  char max[16];
  char last[16];
  char sleepGap[16];
  char acquirePeriod[16];
  char releasePeriod[16];
  formatDurationUsShort(totalUs, total, sizeof(total));
  formatDurationUsShort(totalUs / count, average, sizeof(average));
  formatDurationUsShort(maxUs, max, sizeof(max));
  formatDurationUsShort(lastUs, last, sizeof(last));
  formatDurationUsShort(sleepGapUs, sleepGap, sizeof(sleepGap));
  formatDurationUsShort(acquirePeriodUs, acquirePeriod, sizeof(acquirePeriod));
  formatDurationUsShort(releasePeriodUs, releasePeriod, sizeof(releasePeriod));
  const uint16_t estimatedEvents = estimateConnectionEvents(acquirePeriodUs, intervalUs);

  char line[192];
  snprintf(line, sizeof(line),
           "BTLD: +%llu %s avg%s max%s last%s gap%s acq%s rel%s ev%u lat%u/%u b%llu/%llu/%llu/%llu/%llu",
           static_cast<unsigned long long>(count), total, average, max, last, sleepGap, acquirePeriod, releasePeriod,
           static_cast<unsigned>(estimatedEvents),
           static_cast<unsigned>(estimatedEvents > 0 ? estimatedEvents - 1 : 0), static_cast<unsigned>(latency),
           static_cast<unsigned long long>(buckets[0]), static_cast<unsigned long long>(buckets[1]),
           static_cast<unsigned long long>(buckets[2]), static_cast<unsigned long long>(buckets[3]),
           static_cast<unsigned long long>(buckets[4]));
  return line;
}

extern "C" esp_err_t __wrap_esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg, const char* name,
                                                esp_pm_lock_handle_t* out_handle) {
  const esp_err_t err = __real_esp_pm_lock_create(lock_type, arg, name, out_handle);
  if (err == ESP_OK && out_handle && isBtApbLock(lock_type, name)) {
    trackBtLock(*out_handle);
  }
  return err;
}

extern "C" esp_err_t __wrap_esp_pm_lock_acquire(esp_pm_lock_handle_t handle) {
  const esp_err_t err = __real_esp_pm_lock_acquire(handle);
  recordBtLockAcquire(handle, err);
  return err;
}

extern "C" esp_err_t __wrap_esp_pm_lock_release(esp_pm_lock_handle_t handle) {
  const esp_err_t err = __real_esp_pm_lock_release(handle);
  recordBtLockRelease(handle, err);
  return err;
}

extern "C" esp_err_t __wrap_esp_pm_lock_delete(esp_pm_lock_handle_t handle) {
  untrackBtLock(handle);
  return __real_esp_pm_lock_delete(handle);
}

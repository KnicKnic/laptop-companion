#include "BtSleepTiming.h"

#include "AppLog.h"

#include <esp_attr.h>
#include <esp_private/esp_clk.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/rtc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
esp_err_t __real_esp_timer_create(const esp_timer_create_args_t* create_args, esp_timer_handle_t* out_handle);
esp_err_t __real_esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t __real_esp_timer_stop(esp_timer_handle_t timer);
uint32_t __real_rtc_clk_cal(rtc_cal_sel_t cal_clk, uint32_t slow_clk_cycles);
}

namespace {

// Mirrors components/bt/controller/esp32c3/bt.c (used by ESP32-C3 and ESP32-S3).
constexpr uint32_t kIdfMinUncertaintyUs = 1800;
constexpr uint32_t kIdfRcSlowDriftPercent = 7;
// Mirrors RTC_CLK_SRC_CAL_CYCLES in components/esp_hw_support/sleep_modes.c.
constexpr uint32_t kIdfSleepCalCycles = 10;

constexpr uint32_t kReferenceCalCycles = 1024;
constexpr uint32_t kReferencePeriodMs = 30000;

constexpr int32_t kMissedMarginUs = -2500;  // BTDM_MODEM_WAKE_UP_DELAY is 8 half-slots (2.5 ms).
constexpr int32_t kAutotuneTargetMarginUs = 400;
constexpr uint32_t kAutotuneMinLeadUs = 300;
constexpr uint32_t kAutotuneMaxLeadUs = 20000;
constexpr uint32_t kAutotuneMissBumpUs = 1000;
constexpr uint32_t kAutotuneMaxStepDownUs = 200;
constexpr uint32_t kAutotuneWindow = 16;
constexpr uint32_t kMinTimerTimeoutUs = 500;

DRAM_ATTR portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// btSlp tracking.
DRAM_ATTR esp_timer_handle_t s_btSlpTimer = nullptr;
DRAM_ATTR bool s_rcSlow = false;
DRAM_ATTR bool s_autotune = false;
DRAM_ATTR uint32_t s_autoLeadUs = kIdfMinUncertaintyUs;
DRAM_ATTR bool s_pending = false;
DRAM_ATTR int64_t s_expectedFireAtUs = 0;
DRAM_ATTR int64_t s_controllerWakeAtUs = 0;
DRAM_ATTR uint32_t s_winCount = 0;
DRAM_ATTR int32_t s_winMinMarginUs = INT32_MAX;
DRAM_ATTR uint32_t s_calGuardPpm = 0;
DRAM_ATTR int64_t s_btLockAcquiredAtUs = 0;
DRAM_ATTR int64_t s_statsResetAtUs = 0;

// RTC calibration tracking.
DRAM_ATTR bool s_rtcFilter = false;
DRAM_ATTR int64_t s_emaQ8 = 0;
DRAM_ATTR uint32_t s_calWinCount = 0;
DRAM_ATTR uint32_t s_calWinMin = UINT32_MAX;
DRAM_ATTR uint32_t s_calWinMax = 0;
DRAM_ATTR uint64_t s_calWinSum = 0;

DRAM_ATTR BtSleepTimingStats s_stats;

bool s_begun = false;

inline uint32_t IRAM_ATTR idfUncertaintyFor(uint32_t usToSleep) {
  uint32_t uncertainty = usToSleep >> 11;
  if (s_rcSlow) uncertainty = usToSleep / 100U * kIdfRcSlowDriftPercent;
  return uncertainty < kIdfMinUncertaintyUs ? kIdfMinUncertaintyUs : uncertainty;
}

// bt.c starts btSlp with (us_to_sleep - uncertainty); recover us_to_sleep.
inline uint32_t IRAM_ATTR reconstructUsToSleep(uint32_t timeoutUs) {
  const uint32_t candidate = timeoutUs + kIdfMinUncertaintyUs;
  if (idfUncertaintyFor(candidate) == kIdfMinUncertaintyUs) return candidate;
  if (s_rcSlow) {
    // timeout = us * 93 / 100.
    return timeoutUs < 40000000U ? timeoutUs * 100U / (100U - kIdfRcSlowDriftPercent) : timeoutUs;
  }
  // timeout = us - us/2048 ~= us * 2047/2048.
  return timeoutUs + (timeoutUs >> 11) + 1U;
}

inline uint8_t IRAM_ATTR marginBucket(int32_t marginUs) {
  if (marginUs < kMissedMarginUs) return 0;
  if (marginUs < 0) return 1;
  if (marginUs < 250) return 2;
  if (marginUs < 500) return 3;
  if (marginUs < 1000) return 4;
  if (marginUs < 2000) return 5;
  return 6;
}

void IRAM_ATTR autotuneOnWake(int32_t marginUs) {
  if (!s_autotune) return;
  if (marginUs < 0) {
    s_autoLeadUs = std::min<uint32_t>(s_autoLeadUs + kAutotuneMissBumpUs, kAutotuneMaxLeadUs);
    s_winCount = 0;
    s_winMinMarginUs = INT32_MAX;
    return;
  }
  s_winCount++;
  if (marginUs < s_winMinMarginUs) s_winMinMarginUs = marginUs;
  if (s_winCount < kAutotuneWindow) return;

  // The margin is measured with esp_timer, which is itself re-derived from the
  // RTC slow clock across light sleep, so slow-clock calibration error is
  // invisible to it. Reserve that budget explicitly.
  const uint32_t sleepMs = s_stats.lastSleepUs / 1000U;
  const int32_t calGuardUs = static_cast<int32_t>((s_calGuardPpm * sleepMs) / 1000U);
  const int32_t target = kAutotuneTargetMarginUs + calGuardUs;
  int32_t lead = static_cast<int32_t>(s_autoLeadUs);
  if (s_winMinMarginUs < target) {
    lead += target - s_winMinMarginUs;
  } else {
    lead -= std::min<int32_t>((s_winMinMarginUs - target) / 2, static_cast<int32_t>(kAutotuneMaxStepDownUs));
  }
  if (lead < static_cast<int32_t>(kAutotuneMinLeadUs)) lead = kAutotuneMinLeadUs;
  if (lead > static_cast<int32_t>(kAutotuneMaxLeadUs)) lead = kAutotuneMaxLeadUs;
  s_autoLeadUs = static_cast<uint32_t>(lead);
  s_winCount = 0;
  s_winMinMarginUs = INT32_MAX;
}

int32_t ppmError(uint32_t value, uint32_t reference) {
  if (reference == 0) return 0;
  const int64_t diff = static_cast<int64_t>(value) - static_cast<int64_t>(reference);
  return static_cast<int32_t>(diff * 1000000LL / static_cast<int64_t>(reference));
}

void referenceCalibrationTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kReferencePeriodMs));
    const uint32_t ref = __real_rtc_clk_cal(RTC_CAL_RTC_MUX, kReferenceCalCycles);
    if (ref == 0) continue;
    const uint32_t inUse = esp_clk_slowclk_cal_get();

    portENTER_CRITICAL(&s_mux);
    const uint32_t n = s_calWinCount;
    const uint32_t mn = s_calWinMin;
    const uint32_t mx = s_calWinMax;
    const uint64_t sum = s_calWinSum;
    const int64_t emaQ8 = s_emaQ8;
    s_calWinCount = 0;
    s_calWinMin = UINT32_MAX;
    s_calWinMax = 0;
    s_calWinSum = 0;
    portEXIT_CRITICAL(&s_mux);

    BtSleepTimingStats update;
    update.calSamples = n;
    if (n > 0) {
      update.calMinPpm = ppmError(mn, ref);
      update.calMaxPpm = ppmError(mx, ref);
      update.calMeanPpm = ppmError(static_cast<uint32_t>(sum / n), ref);
    }
    update.calEmaPpm = emaQ8 > 0 ? ppmError(static_cast<uint32_t>((emaQ8 + 128) >> 8), ref) : ppmError(inUse, ref);
    // 2^19 us-fraction bits per period (RTC_CLK_CAL_FRACT).
    update.refSlowClockHz = static_cast<uint32_t>((1000000ULL << 19) / ref);

    portENTER_CRITICAL(&s_mux);
    s_stats.calSamples = update.calSamples;
    s_stats.calMinPpm = update.calMinPpm;
    s_stats.calMaxPpm = update.calMaxPpm;
    s_stats.calMeanPpm = update.calMeanPpm;
    s_stats.calEmaPpm = update.calEmaPpm;
    s_stats.refSlowClockHz = update.refSlowClockHz;
    s_stats.refCount++;
    if (n > 0) {
      const int32_t worst = std::max(std::abs(update.calMinPpm), std::abs(update.calMaxPpm));
      if (worst > std::abs(s_stats.calWorstPpm)) {
        s_stats.calWorstPpm = std::abs(update.calMinPpm) > std::abs(update.calMaxPpm) ? update.calMinPpm
                                                                                       : update.calMaxPpm;
      }
      // What the wake timing actually uses: the filtered value when filtering,
      // otherwise any raw sample.
      const uint32_t guard = s_rtcFilter ? static_cast<uint32_t>(std::abs(update.calEmaPpm)) + 50U
                                         : static_cast<uint32_t>(worst) + 50U;
      s_calGuardPpm = guard;
      s_stats.calGuardPpm = guard;
    }
    portEXIT_CRITICAL(&s_mux);

    logPrintf("%s\n", formatRtcCalLine().c_str());
    logPrintf("%s\n", formatBtWakeLine().c_str());
  }
}

}  // namespace

extern "C" esp_err_t __wrap_esp_timer_create(const esp_timer_create_args_t* create_args,
                                             esp_timer_handle_t* out_handle) {
  const esp_err_t err = __real_esp_timer_create(create_args, out_handle);
  if (err == ESP_OK && create_args && create_args->name && out_handle && strcmp(create_args->name, "btSlp") == 0) {
    s_btSlpTimer = *out_handle;
  }
  return err;
}

extern "C" esp_err_t IRAM_ATTR __wrap_esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
  if (timer == nullptr || timer != s_btSlpTimer || timeout_us > UINT32_MAX) {
    return __real_esp_timer_start_once(timer, timeout_us);
  }

  const int64_t nowUs = esp_timer_get_time();
  const uint32_t timeoutUs = static_cast<uint32_t>(timeout_us);
  const uint32_t usToSleep = reconstructUsToSleep(timeoutUs);
  const uint32_t defaultLead = usToSleep > timeoutUs ? usToSleep - timeoutUs : 0;

  portENTER_CRITICAL_SAFE(&s_mux);
  uint32_t lead = s_autotune ? s_autoLeadUs : defaultLead;
  uint32_t adjusted = timeoutUs;
  if (s_autotune && usToSleep > lead + kMinTimerTimeoutUs) {
    adjusted = usToSleep - lead;
  } else {
    lead = defaultLead;
  }
  s_pending = true;
  s_expectedFireAtUs = nowUs + adjusted;
  s_controllerWakeAtUs = nowUs + usToSleep;
  s_stats.btSlpArmed++;
  s_stats.lastSleepUs = usToSleep;
  s_stats.defaultLeadUs = defaultLead;
  s_stats.leadUs = lead;
  portEXIT_CRITICAL_SAFE(&s_mux);

  return __real_esp_timer_start_once(timer, adjusted);
}

extern "C" esp_err_t IRAM_ATTR __wrap_esp_timer_stop(esp_timer_handle_t timer) {
  const esp_err_t err = __real_esp_timer_stop(timer);
  if (timer != nullptr && timer == s_btSlpTimer) {
    portENTER_CRITICAL_SAFE(&s_mux);
    // Stopped without the BT lock having been taken (e.g. controller disabled).
    s_pending = false;
    portEXIT_CRITICAL_SAFE(&s_mux);
  }
  return err;
}

extern "C" uint32_t IRAM_ATTR __wrap_rtc_clk_cal(rtc_cal_sel_t cal_clk, uint32_t slow_clk_cycles) {
  const uint32_t raw = __real_rtc_clk_cal(cal_clk, slow_clk_cycles);
  // Only the short per-light-sleep calibration from sleep_modes.c is touched;
  // the boot-time CONFIG_RTC_CLK_CAL_CYCLES calibration passes straight through.
  if (raw == 0 || cal_clk != RTC_CAL_RTC_MUX || slow_clk_cycles != kIdfSleepCalCycles) return raw;

  portENTER_CRITICAL_SAFE(&s_mux);
  if (s_emaQ8 == 0) {
    s_emaQ8 = static_cast<int64_t>(raw) << 8;
  } else {
    s_emaQ8 += ((static_cast<int64_t>(raw) << 8) - s_emaQ8) >> 4;
  }
  s_calWinCount++;
  s_calWinSum += raw;
  if (raw < s_calWinMin) s_calWinMin = raw;
  if (raw > s_calWinMax) s_calWinMax = raw;
  s_stats.calSamplesTotal++;
  const bool filter = s_rtcFilter;
  const uint32_t filtered = static_cast<uint32_t>((s_emaQ8 + 128) >> 8);
  portEXIT_CRITICAL_SAFE(&s_mux);
  return filter ? filtered : raw;
}

void btSleepTimingOnBtLockAcquired(int64_t nowUs) {
  portENTER_CRITICAL_SAFE(&s_mux);
  s_btLockAcquiredAtUs = nowUs;
  if (!s_pending) {
    s_stats.unscheduledWakes++;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return;
  }
  s_pending = false;
  const int64_t margin64 = s_controllerWakeAtUs - nowUs;
  const int32_t marginUs = margin64 > INT32_MAX ? INT32_MAX : (margin64 < INT32_MIN ? INT32_MIN : margin64);
  const bool timerPath = nowUs >= s_expectedFireAtUs;
  s_stats.wakes++;
  if (timerPath) {
    s_stats.timerWakes++;
  } else {
    s_stats.preemptedWakes++;
  }
  if (marginUs < 0) s_stats.lateWakes++;
  if (marginUs < kMissedMarginUs) s_stats.missedWakes++;
  if (s_stats.wakes == 1 || marginUs < s_stats.minMarginUs) s_stats.minMarginUs = marginUs;
  s_stats.lastMarginUs = marginUs;
  s_stats.marginBuckets[marginBucket(marginUs)]++;
  // Only timer-driven wakes say anything about the lead; a preempted wake
  // (host traffic, controller-internal) happened before btSlp was due.
  if (timerPath) {
    s_stats.marginSumUs += marginUs;
    autotuneOnWake(marginUs);
  }
  portEXIT_CRITICAL_SAFE(&s_mux);
}

void btSleepTimingOnBtLockReleased(int64_t nowUs) {
  portENTER_CRITICAL_SAFE(&s_mux);
  if (s_btLockAcquiredAtUs > 0 && nowUs > s_btLockAcquiredAtUs) {
    s_stats.btAwakeUs += static_cast<uint64_t>(nowUs - s_btLockAcquiredAtUs);
  }
  s_btLockAcquiredAtUs = 0;
  portEXIT_CRITICAL_SAFE(&s_mux);
}

void beginBtSleepTiming() {
  if (s_begun) return;
  s_begun = true;
  s_rcSlow = rtc_clk_slow_src_get() == SOC_RTC_SLOW_CLK_SRC_RC_SLOW;
  s_stats.rtcSlowIsRc = s_rcSlow;
  s_statsResetAtUs = esp_timer_get_time();
  if (xTaskCreate(referenceCalibrationTask, "rtc-cal-ref", 3072, nullptr, 1, nullptr) != pdPASS) {
    logPrintf("BtSleepTiming: failed to start reference calibration task\n");
  }
  logPrintf("BtSleepTiming: slow clock %s, boot cal %lu\n", s_rcSlow ? "RC_SLOW (7%% BT window)" : "non-RC",
            static_cast<unsigned long>(esp_clk_slowclk_cal_get()));
}

void setBtWakeAutotuneEnabled(bool enabled) {
  portENTER_CRITICAL(&s_mux);
  if (enabled && !s_autotune) {
    // Start from IDF's own lead and walk down from there.
    s_autoLeadUs = s_stats.defaultLeadUs > 0 ? s_stats.defaultLeadUs : kIdfMinUncertaintyUs;
    s_winCount = 0;
    s_winMinMarginUs = INT32_MAX;
  }
  s_autotune = enabled;
  portEXIT_CRITICAL(&s_mux);
}

void setRtcCalFilterEnabled(bool enabled) {
  portENTER_CRITICAL(&s_mux);
  s_rtcFilter = enabled;
  portEXIT_CRITICAL(&s_mux);
}

void resetBtSleepTimingStats() {
  portENTER_CRITICAL(&s_mux);
  const BtSleepTimingStats previous = s_stats;
  s_stats = BtSleepTimingStats{};
  s_stats.rtcSlowIsRc = previous.rtcSlowIsRc;
  s_stats.calGuardPpm = previous.calGuardPpm;
  s_stats.refSlowClockHz = previous.refSlowClockHz;
  s_statsResetAtUs = esp_timer_get_time();
  portEXIT_CRITICAL(&s_mux);
}

BtSleepTimingStats copyBtSleepTimingStats() {
  portENTER_CRITICAL(&s_mux);
  BtSleepTimingStats copy = s_stats;
  copy.autotune = s_autotune;
  copy.rtcFilter = s_rtcFilter;
  const int64_t resetAt = s_statsResetAtUs;
  const int64_t lockAt = s_btLockAcquiredAtUs;
  portEXIT_CRITICAL(&s_mux);
  const int64_t nowUs = esp_timer_get_time();
  copy.wallUs = nowUs > resetAt ? static_cast<uint64_t>(nowUs - resetAt) : 0;
  if (lockAt > 0 && nowUs > lockAt) copy.btAwakeUs += static_cast<uint64_t>(nowUs - lockAt);
  return copy;
}

std::string formatBtWakeLine() {
  const BtSleepTimingStats s = copyBtSleepTimingStats();
  const int32_t avg = s.timerWakes > 0 ? static_cast<int32_t>(s.marginSumUs / static_cast<int64_t>(s.timerWakes)) : 0;
  const unsigned btPermille =
      s.wallUs > 0 ? static_cast<unsigned>((s.btAwakeUs * 1000ULL) / s.wallUs) : 0;
  char line[200];
  snprintf(line, sizeof(line),
           "BTW: n%lu tmr%lu pre%lu late%lu miss%lu min%ldus avg%ldus lead%lu/%luus%s bt%u.%u%% "
           "b%lu/%lu/%lu/%lu/%lu/%lu/%lu",
           static_cast<unsigned long>(s.wakes), static_cast<unsigned long>(s.timerWakes),
           static_cast<unsigned long>(s.preemptedWakes), static_cast<unsigned long>(s.lateWakes),
           static_cast<unsigned long>(s.missedWakes), static_cast<long>(s.minMarginUs), static_cast<long>(avg),
           static_cast<unsigned long>(s.leadUs), static_cast<unsigned long>(s.defaultLeadUs),
           s.autotune ? " auto" : "", btPermille / 10U, btPermille % 10U,
           static_cast<unsigned long>(s.marginBuckets[0]), static_cast<unsigned long>(s.marginBuckets[1]),
           static_cast<unsigned long>(s.marginBuckets[2]), static_cast<unsigned long>(s.marginBuckets[3]),
           static_cast<unsigned long>(s.marginBuckets[4]), static_cast<unsigned long>(s.marginBuckets[5]),
           static_cast<unsigned long>(s.marginBuckets[6]));
  return line;
}

std::string formatRtcCalLine() {
  const BtSleepTimingStats s = copyBtSleepTimingStats();
  char line[200];
  snprintf(line, sizeof(line),
           "RTC: %luHz%s n%lu raw%+ld..%+ldppm avg%+ld ema%+ld worst%+ld guard%lu filt:%s",
           static_cast<unsigned long>(s.refSlowClockHz), s.rtcSlowIsRc ? " RC" : "",
           static_cast<unsigned long>(s.calSamples), static_cast<long>(s.calMinPpm),
           static_cast<long>(s.calMaxPpm), static_cast<long>(s.calMeanPpm), static_cast<long>(s.calEmaPpm),
           static_cast<long>(s.calWorstPpm), static_cast<unsigned long>(s.calGuardPpm), s.rtcFilter ? "on" : "off");
  return line;
}

std::string formatBtSleepTimingKv() {
  const BtSleepTimingStats s = copyBtSleepTimingStats();
  const int32_t avg = s.timerWakes > 0 ? static_cast<int32_t>(s.marginSumUs / static_cast<int64_t>(s.timerWakes)) : 0;
  const unsigned btPermille =
      s.wallUs > 0 ? static_cast<unsigned>((s.btAwakeUs * 1000ULL) / s.wallUs) : 0;
  char kv[480];
  snprintf(kv, sizeof(kv),
           "win_s=%lu armed=%lu wakes=%lu tmr=%lu pre=%lu unsched=%lu late=%lu miss=%lu min_us=%ld avg_us=%ld "
           "last_us=%ld lead_us=%lu idf_lead_us=%lu sleep_us=%lu tune=%u bt_pm=%u "
           "mb=%lu/%lu/%lu/%lu/%lu/%lu/%lu "
           "cal_hz=%lu cal_n=%lu cal_min=%ld cal_max=%ld cal_avg=%ld cal_ema=%ld cal_worst=%ld guard=%lu rtcf=%u rc=%u",
           static_cast<unsigned long>(s.wallUs / 1000000ULL), static_cast<unsigned long>(s.btSlpArmed),
           static_cast<unsigned long>(s.wakes), static_cast<unsigned long>(s.timerWakes),
           static_cast<unsigned long>(s.preemptedWakes), static_cast<unsigned long>(s.unscheduledWakes),
           static_cast<unsigned long>(s.lateWakes), static_cast<unsigned long>(s.missedWakes),
           static_cast<long>(s.minMarginUs), static_cast<long>(avg), static_cast<long>(s.lastMarginUs),
           static_cast<unsigned long>(s.leadUs), static_cast<unsigned long>(s.defaultLeadUs),
           static_cast<unsigned long>(s.lastSleepUs), s.autotune ? 1U : 0U, btPermille,
           static_cast<unsigned long>(s.marginBuckets[0]), static_cast<unsigned long>(s.marginBuckets[1]),
           static_cast<unsigned long>(s.marginBuckets[2]), static_cast<unsigned long>(s.marginBuckets[3]),
           static_cast<unsigned long>(s.marginBuckets[4]), static_cast<unsigned long>(s.marginBuckets[5]),
           static_cast<unsigned long>(s.marginBuckets[6]), static_cast<unsigned long>(s.refSlowClockHz),
           static_cast<unsigned long>(s.calSamples), static_cast<long>(s.calMinPpm),
           static_cast<long>(s.calMaxPpm), static_cast<long>(s.calMeanPpm), static_cast<long>(s.calEmaPpm),
           static_cast<long>(s.calWorstPpm), static_cast<unsigned long>(s.calGuardPpm), s.rtcFilter ? 1U : 0U,
           s.rtcSlowIsRc ? 1U : 0U);
  return kv;
}

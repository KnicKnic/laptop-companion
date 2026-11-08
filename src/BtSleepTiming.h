#pragma once

#include <cstdint>
#include <string>

// Instrumentation and optional self-calibration for the ESP32-S3 BLE controller's
// light-sleep wake path.
//
// With CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1 + CONFIG_PM_ENABLE, IDF's bt.c arms an
// esp_timer named "btSlp" before every controller sleep. It fires `uncertainty`
// microseconds before the controller's own (main XTAL timed) wake-up so the CPU
// is out of light sleep and holding the "bt" PM lock in time:
//   uncertainty = max(us_to_sleep / 2048, 1800 us)     (8MD256 / XTAL slow clock)
//   uncertainty = max(us_to_sleep * 7 / 100, 1800 us)  (136 kHz RC_SLOW clock)
// The CPU's light-sleep wake is timed by the RTC slow clock, which IDF
// re-calibrates every light sleep using only 10 slow-clock cycles.
//
// This module wraps (via -Wl,--wrap) esp_timer_create/start_once/stop and
// rtc_clk_cal to measure:
//   * BT wake margin: how far ahead of the controller's wake the CPU actually
//     acquired the "bt" PM lock (negative = the CPU was late).
//   * RTC slow-clock calibration error: each 10-cycle per-sleep calibration is
//     compared against a periodic long (1024-cycle) reference calibration.
// and can optionally:
//   * auto-tune the btSlp lead time from the measured margins (instead of the
//     fixed 1.8 ms / 7% window), and
//   * low-pass filter the per-sleep slow clock calibration.

struct BtSleepTimingStats {
  // BT wake path.
  uint32_t btSlpArmed = 0;          // btSlp timer arms seen.
  uint32_t wakes = 0;               // "bt" lock acquisitions matched to an armed btSlp.
  uint32_t timerWakes = 0;          // ...where btSlp had already fired (normal path).
  uint32_t preemptedWakes = 0;      // ...where the controller/host woke BT before btSlp fired.
  uint32_t unscheduledWakes = 0;    // "bt" lock acquisitions with no armed btSlp.
  uint32_t lateWakes = 0;           // margin < 0: CPU acquired the lock after controller wake.
  uint32_t missedWakes = 0;         // margin < -2500 us: past the modem wake-up delay; event very likely missed.
  int32_t minMarginUs = 0;
  int32_t lastMarginUs = 0;
  int64_t marginSumUs = 0;          // Sum over timer-path wakes (avg excludes preempted wakes).
  uint32_t marginBuckets[7] = {};   // <-2500, <0, <250, <500, <1000, <2000, >=2000 us.
  uint32_t lastSleepUs = 0;         // Last controller sleep duration (us_to_sleep).
  uint32_t defaultLeadUs = 0;       // IDF's uncertainty for the last sleep.
  uint32_t leadUs = 0;              // Lead actually used for the last sleep.
  bool autotune = false;
  uint64_t btAwakeUs = 0;           // Time the "bt" PM lock was held.
  uint64_t wallUs = 0;              // Wall time since stats reset.

  // RTC slow clock calibration.
  bool rtcFilter = false;
  bool rtcSlowIsRc = false;
  uint32_t calSamples = 0;          // Per-sleep 10-cycle calibrations seen since last reference.
  uint32_t calSamplesTotal = 0;
  int32_t calMinPpm = 0;            // Raw per-sleep calibration error vs reference (last window).
  int32_t calMaxPpm = 0;
  int32_t calMeanPpm = 0;
  int32_t calEmaPpm = 0;            // Filtered value vs reference.
  int32_t calWorstPpm = 0;          // Largest |raw error| since boot / reset.
  uint32_t calGuardPpm = 0;         // Error budget used by the auto-tuner.
  uint32_t refSlowClockHz = 0;      // From the latest reference calibration.
  uint32_t refCount = 0;
};

void beginBtSleepTiming();
void setBtWakeAutotuneEnabled(bool enabled);
void setRtcCalFilterEnabled(bool enabled);
void resetBtSleepTimingStats();
BtSleepTimingStats copyBtSleepTimingStats();

// Called by the PM-lock wrappers in PowerStats.cpp when the BT controller's
// "bt" PM lock goes 0 -> 1 (controller awake) and 1 -> 0 (controller asleep).
void btSleepTimingOnBtLockAcquired(int64_t nowUs);
void btSleepTimingOnBtLockReleased(int64_t nowUs);

std::string formatBtWakeLine();
std::string formatRtcCalLine();
// Space-separated key=value tokens; consumed by ble_sleep_soak/.
std::string formatBtSleepTimingKv();

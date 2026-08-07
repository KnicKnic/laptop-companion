#include "OtherTestPage.h"

#include "AppLog.h"
#include "AppState.h"
#include "DisplayWorker.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "Settings.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/rtc.h>

namespace {

constexpr uint32_t kCrystalStartupDelayMs = 1500;
// A watch crystal can take noticeably longer than an RC oscillator to start.
constexpr uint32_t kCrystalSettleDelayMs = 1000;
constexpr uint32_t kCalibrationCycles = 1024;
constexpr uint32_t kMinCrystalHz = 32000;
constexpr uint32_t kMaxCrystalHz = 33500;

uint32_t calibrationToHz(uint32_t calibration) {
  if (calibration == 0) return 0;
  return static_cast<uint32_t>((1000000ULL << RTC_CLK_CAL_FRACT) / calibration);
}

const char* rtcSlowClockSourceText() {
  switch (rtc_clk_slow_src_get()) {
    case SOC_RTC_SLOW_CLK_SRC_XTAL32K:
      return "XTAL32K: crystal on GPIO15 + GPIO16";
    case SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256:
      return "RC_FAST/256: internal oscillator";
    case SOC_RTC_SLOW_CLK_SRC_RC_SLOW:
      return "RC_SLOW: internal 136 kHz RC fallback";
    default:
      return "unknown RTC clock source";
  }
}

const char* bleLowPowerClockBuildText() {
#if CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL
  return "BLE LP build source: external 32.768 kHz XTAL";
#elif CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL
  return "BLE LP build source: main XTAL";
#elif CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW
  return "BLE LP build source: RTC_SLOW_CLK";
#else
  return "BLE LP build source: not configured";
#endif
}

}  // namespace

OtherTestPage::OtherTestPage(EInkDisplay& display) : Page(display) {}

PageId OtherTestPage::id() const {
  return PageId::OtherTest;
}

const char* OtherTestPage::name() const {
  return "other-test";
}

void OtherTestPage::onEnter() {
  for (std::atomic<uint32_t>& value : calibrationValues_) value.store(0, std::memory_order_relaxed);
  crystalTestState_.store(CrystalTestState::WaitingToStart, std::memory_order_release);
  crystalTestRequested_.store(true, std::memory_order_release);
  crystalTestGeneration_.fetch_add(1, std::memory_order_acq_rel);

  bool expected = false;
  if (crystalTestTaskRunning_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    if (xTaskCreate(crystalTestTask, "32k-crystal-test", 4096, this, 1, nullptr) != pdPASS) {
      crystalTestTaskRunning_.store(false, std::memory_order_release);
      crystalTestState_.store(CrystalTestState::Failed, std::memory_order_release);
      logPrintf("32 kHz crystal test: unable to create diagnostic task.\n");
    }
  }
}

void OtherTestPage::onLeave() {
  crystalTestRequested_.store(false, std::memory_order_release);
  crystalTestGeneration_.fetch_add(1, std::memory_order_acq_rel);
  crystalTestState_.store(CrystalTestState::Idle, std::memory_order_release);
}

void OtherTestPage::crystalTestTask(void* context) {
  static_cast<OtherTestPage*>(context)->runCrystalTest();
  vTaskDelete(nullptr);
}

void OtherTestPage::requestCrystalTestRender() const {
  if (!displayWorkerReady() || activePage() != PageId::OtherTest) return;
  requestRender(RenderKind::ActivePage, refreshModeFromSettings(copySettings()));
}

void OtherTestPage::runCrystalTest() {
  // Keep one task alive long enough to handle a quick page leave/re-enter without
  // ever running more than one RTC calibration at a time.
  for (;;) {
    const uint32_t generation = crystalTestGeneration_.load(std::memory_order_acquire);
    vTaskDelay(pdMS_TO_TICKS(kCrystalStartupDelayMs));

    if (!crystalTestRequested_.load(std::memory_order_acquire)) {
      crystalTestTaskRunning_.store(false, std::memory_order_release);
      return;
    }
    if (generation != crystalTestGeneration_.load(std::memory_order_acquire)) continue;

    crystalTestState_.store(CrystalTestState::Measuring, std::memory_order_release);
    requestCrystalTestRender();

    // This probes the XTAL32K input directly.  It deliberately does not select
    // XTAL32K as RTC_SLOW_CLK, so CONFIG_RTC_CLK_SRC_INT_8MD256 remains in use.
    rtc_clk_32k_enable(true);
    vTaskDelay(pdMS_TO_TICKS(kCrystalSettleDelayMs));

    bool valid = true;
    for (uint8_t i = 0; i < kCalibrationSamples; ++i) {
      const uint32_t calibration = rtc_clk_cal(RTC_CAL_32K_XTAL, kCalibrationCycles);
      calibrationValues_[i].store(calibration, std::memory_order_relaxed);
      const uint32_t frequencyHz = calibrationToHz(calibration);
      if (frequencyHz < kMinCrystalHz || frequencyHz > kMaxCrystalHz) valid = false;
    }
    rtc_clk_32k_enable(false);

    if (crystalTestRequested_.load(std::memory_order_acquire) &&
        generation == crystalTestGeneration_.load(std::memory_order_acquire)) {
      crystalTestState_.store(valid ? CrystalTestState::Passed : CrystalTestState::Failed,
                              std::memory_order_release);
      logPrintf("32 kHz crystal test: %s. Samples: %lu, %lu, %lu Hz\n", valid ? "PASS" : "FAIL",
                static_cast<unsigned long>(calibrationToHz(calibrationValues_[0].load(std::memory_order_relaxed))),
                static_cast<unsigned long>(calibrationToHz(calibrationValues_[1].load(std::memory_order_relaxed))),
                static_cast<unsigned long>(calibrationToHz(calibrationValues_[2].load(std::memory_order_relaxed))));
      requestCrystalTestRender();
      crystalTestTaskRunning_.store(false, std::memory_order_release);
      return;
    }
  }
}

std::unique_ptr<RenderTransaction> OtherTestPage::render(freeink::ui::DisplayTarget& target,
                                                         EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  const AppState state = copyAppState();

  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 2;

  const CrystalTestState crystalTestState = crystalTestState_.load(std::memory_order_acquire);
  char crystalStatus[160];
  switch (crystalTestState) {
    case CrystalTestState::WaitingToStart:
      snprintf(crystalStatus, sizeof(crystalStatus),
               "32 kHz crystal diagnostic: this page will start the test shortly.");
      break;
    case CrystalTestState::Measuring:
      snprintf(crystalStatus, sizeof(crystalStatus), "32 kHz crystal diagnostic: measuring oscillator stability...");
      break;
    case CrystalTestState::Passed: {
      const uint32_t firstHz = calibrationToHz(calibrationValues_[0].load(std::memory_order_relaxed));
      const uint32_t secondHz = calibrationToHz(calibrationValues_[1].load(std::memory_order_relaxed));
      const uint32_t thirdHz = calibrationToHz(calibrationValues_[2].load(std::memory_order_relaxed));
      snprintf(crystalStatus, sizeof(crystalStatus), "PASS: working 32 kHz clock detected (%lu, %lu, %lu Hz).",
               static_cast<unsigned long>(firstHz), static_cast<unsigned long>(secondHz),
               static_cast<unsigned long>(thirdHz));
      break;
    }
    case CrystalTestState::Failed:
      snprintf(crystalStatus, sizeof(crystalStatus),
               "FAIL: no stable 32 kHz clock detected. Keep INT_8MD256 selected.");
      break;
    case CrystalTestState::Idle:
      snprintf(crystalStatus, sizeof(crystalStatus), "32 kHz crystal diagnostic is inactive.");
      break;
  }

  char navigationLine[96];
  snprintf(navigationLine, sizeof(navigationLine), "Route: /other/test");
  char statusLine[96];
  snprintf(statusLine, sizeof(statusLine), "Runtime: %s   SD:%s   Settings:%s", tx->isX3Mode() ? "X3" : "X4",
           state.sdReady ? "ready" : "missing", state.companionSettingsValid ? "ready" : "warning");
  char rtcClockLine[128];
  snprintf(rtcClockLine, sizeof(rtcClockLine), "RTC_SLOW_CLK now: %s", rtcSlowClockSourceText());

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 24), content.width, 44},
                        "32 kHz Crystal Test", title);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 116), content.width, 44},
                        crystalStatus, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 186), content.width, 44},
                        "Test pins: GPIO15 (XTAL_32K_P), GPIO16 (XTAL_32K_N).", body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 246), content.width, 44},
                        rtcClockLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 306), content.width, 44},
                        bleLowPowerClockBuildText(), body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 72), content.width, 44},
                        statusLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 34), content.width, 24},
                        "/other/test", body);
  drawPageChrome(target);
  return tx;
}

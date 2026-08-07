#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <PowerManager.h>
#include <RecoveryBoot.h>
#include <SDCardManager.h>
#include <XteinkDetect.h>

#include <esp_intr_alloc.h>
#include <esp_pm.h>
#include <esp_private/pm_impl.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include "AppLog.h"
#include "AppSdBus.h"
#include "AppState.h"
#include "DisplayWorker.h"
#include "IsrInput.h"
#include "InterruptStats.h"
#include "LinkerQuirks.h"
#include "PageManager.h"
#include "PowerManagement.h"
#include "PowerStats.h"
#include "Settings.h"

namespace {

EInkDisplay* display = nullptr;

constexpr uint32_t kMissingSettingsDelayMs = 20000;
constexpr uint32_t kBootMaxPowerMs = 10000;
constexpr uint32_t kButtonMaxPowerMs = 1000;
constexpr uint32_t kPmStatsDumpPeriodMs = 15000;

#if CONFIG_PM_PROFILING
void pmStatsDumpTask(void*) {
  InterruptStatsSnapshot previousInterruptStats;
  uint32_t previousTouchIrqCount = 0;
  uint32_t previousButtonIrqCount = 0;
  uint32_t previousEpdBusyIrqCount = 0;
  bool havePreviousInterruptStats = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPmStatsDumpPeriodMs));
    const InterruptStatsSnapshot interruptStats = copyInterruptStats();
    const uint32_t touchIrqCount = inputTouchInterruptCount();
    const uint32_t buttonIrqCount = inputButtonInterruptCount();
    const uint32_t epdBusyIrqCount = freeink::epdBusyInterruptCount();
    Serial.println("--- interrupt counters ---");
    Serial.printf("Input backend: %s\n", inputBackendName());
    Serial.printf("FreeRTOS tick CPU0 total %lu delta +%lu; CPU1 total %lu delta +%lu\n",
                  static_cast<unsigned long>(interruptStats.freertosTickCpu0),
                  static_cast<unsigned long>(havePreviousInterruptStats
                                                 ? interruptStats.freertosTickCpu0 - previousInterruptStats.freertosTickCpu0
                                                 : 0),
                  static_cast<unsigned long>(interruptStats.freertosTickCpu1),
                  static_cast<unsigned long>(havePreviousInterruptStats
                                                 ? interruptStats.freertosTickCpu1 - previousInterruptStats.freertosTickCpu1
                                                 : 0));
    Serial.printf("X4 touch GPIO IRQ total %lu delta +%lu; button GPIO IRQ total %lu delta +%lu\n",
                  static_cast<unsigned long>(touchIrqCount),
                  static_cast<unsigned long>(havePreviousInterruptStats ? touchIrqCount - previousTouchIrqCount : 0),
                  static_cast<unsigned long>(buttonIrqCount),
                  static_cast<unsigned long>(havePreviousInterruptStats ? buttonIrqCount - previousButtonIrqCount : 0));
    Serial.printf("EPD BUSY GPIO IRQ total %lu delta +%lu\n", static_cast<unsigned long>(epdBusyIrqCount),
                  static_cast<unsigned long>(havePreviousInterruptStats ? epdBusyIrqCount - previousEpdBusyIrqCount : 0));
    previousInterruptStats = interruptStats;
    previousTouchIrqCount = touchIrqCount;
    previousButtonIrqCount = buttonIrqCount;
    previousEpdBusyIrqCount = epdBusyIrqCount;
    havePreviousInterruptStats = true;
    Serial.println("\n--- esp_pm_impl_dump_stats ---");
    esp_pm_impl_dump_stats(stdout);
    fflush(stdout);
    Serial.println("--- esp_pm_dump_locks ---");
    const esp_err_t lockDumpResult = esp_pm_dump_locks(stdout);
    if (lockDumpResult != ESP_OK) {
      Serial.printf("esp_pm_dump_locks failed: %s\n", esp_err_to_name(lockDumpResult));
    }
    fflush(stdout);
    /*Serial.println("--- esp_intr_dump ---");
    const esp_err_t intrDumpResult = esp_intr_dump(stdout);
    if (intrDumpResult != ESP_OK) {
      Serial.printf("esp_intr_dump failed: %s\n", esp_err_to_name(intrDumpResult));
    }
    fflush(stdout);*/
    Serial.println("--- esp_timer_dump ---");
    const esp_err_t timerDumpResult = esp_timer_dump(stdout);
    if (timerDumpResult != ESP_OK) {
      Serial.printf("esp_timer_dump failed: %s\n", esp_err_to_name(timerDumpResult));
    }
    fflush(stdout);
    Serial.println("--- end PM stats ---");
  }
}

void startPmStatsDumpTask() {
  const BaseType_t result = xTaskCreate(pmStatsDumpTask, "pm-stats", 4096, nullptr, 1, nullptr);
  if (result != pdPASS) Serial.println("Failed to start PM stats dump task");
}
#else
void startPmStatsDumpTask() {}
#endif

PageId startupPageFromSettings(const CompanionSettings& settings) {
  const String& startup = settings.system.page.startup;
  if (startup == "/" || startup == "main") return PageId::Main;
  if (startup == "/companion" || startup == "companion") return PageId::Companion;
  if (startup == "/companion/stats") return PageId::CompanionStats;
  if (startup == "/settings" || startup == "settings") return PageId::Settings;
  if (startup == "/other/test") return PageId::OtherTest;
  if (startup == "/other/power-stats" || startup == "power_stats") return PageId::PowerStats;
  if (startup == "/other/error") return PageId::CompanionSettingsWarning;
  return PageId::Companion;
}

void enterDeepSleepAfterScreenRender() {
  setAppSleeping(true);
  const uint32_t sleepRender = requestRender(RenderKind::Sleep, EInkDisplay::FULL_REFRESH);
  logPrintf("Queued sleep render request %lu; waiting for exact completion.\n",
            static_cast<unsigned long>(sleepRender));
  waitForRender(sleepRender, portMAX_DELAY);
  logPrintf("Sleep render request %lu completed; entering MCU deep sleep.\n",
            static_cast<unsigned long>(sleepRender));

  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::waitForPowerButtonRelease();
  freeink::PowerManager::armPowerButtonWakeup();
  freeink::PowerManager::deepSleep();
}

bool dynamicPowerManagementConfigured() {
  return copyPowerManagementState().configured;
}

void armMaxPowerIfConfigured(uint32_t milliseconds) {
  if (!dynamicPowerManagementConfigured()) return;
  maxPowerLock().ensureArmed(milliseconds);
}

uint32_t maxPowerMillisecondsLeft() {
  if (!dynamicPowerManagementConfigured()) return 0;
  return maxPowerLock().millisecondsLeft();
}

TickType_t inputWaitTicksForMaxPowerWindow(uint32_t millisecondsLeft) {
  if (millisecondsLeft == 0) return portMAX_DELAY;
  const TickType_t ticks = pdMS_TO_TICKS(millisecondsLeft);
  return ticks > 0 ? ticks : 1;
}

}  // namespace

void setup() {
  freeink::recovery::checkBootCombo();
  Serial.begin(115200);
  keepLinkerQuirkSections();
  delay(200);

  BoardConfig::holdPowerRails();

  const bool detectedX3 = freeink::selectXteinkDevice();
  freeink::applyXteinkDisplayController();
  BoardConfig::releaseSdRail();

  if (!beginAppState()) {
    logPrintf("Failed to create app state mutex.\n");
    while (true) delay(1000);
  }
  if (!beginSettings()) {
    logPrintf("Failed to create settings mutex.\n");
    while (true) delay(1000);
  }

  const bool sdReady = SdMan.begin();
  setSdReady(sdReady);
  const SettingsLoadResult settingsResult = loadSettingsFromSd();
  const bool companionSettingsPresent = settingsResult == SettingsLoadResult::Loaded;
  const bool companionSettingsValid = settingsResult == SettingsLoadResult::Loaded;
  setCompanionSettingsStatus(true, companionSettingsPresent, companionSettingsValid);
  setSerialLoggingEnabled(copySettings().system.debug.serialLogging);

  PageId bootPage = PageId::Companion;
  if (companionSettingsValid) {
    bootPage = startupPageFromSettings(copySettings());
  } else {
    logPrintf("%s %s; delaying %lu ms before warning screen.\n", kCompanionSettingsPath,
              settingsLoadResultName(settingsResult),
              static_cast<unsigned long>(kMissingSettingsDelayMs));
    delay(kMissingSettingsDelayMs);
    bootPage = PageId::CompanionSettingsWarning;
  }

  if (sdReady) {
    dumpSdDirectory("/");
    dumpSdDirectory("/sleep");
  }

  configurePowerManagementFromSettings();
  armMaxPowerIfConfigured(kBootMaxPowerMs);
  beginPowerStats();
  startPmStatsDumpTask();

  display = new EInkDisplay(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
                            BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
                            BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
  if (detectedX3) {
    display->setDisplayX3();
  }

  display->begin();
  if (!beginPageManager(*display, bootPage)) {
    logPrintf("Failed to create page manager mutex.\n");
    while (true) delay(1000);
  }
  beginInput();
  if (!beginDisplayWorker(*display)) {
    logPrintf("Failed to create FreeRTOS display synchronization primitives.\n");
    while (true) delay(1000);
  }

  const uint32_t bootRender = requestRender(RenderKind::ActivePage, EInkDisplay::FULL_REFRESH);
  waitForRender(bootRender, portMAX_DELAY);

  const AppState bootState = copyAppState();
  logPrintf("FreeInk hello world displayed on %s (%dx%d), SD %s\n", BoardConfig::ACTIVE.name,
            display->getDisplayWidth(), display->getDisplayHeight(), bootState.sdReady ? "ready" : "not ready");
}

void loop() {
  ButtonPress press;
  const uint32_t maxPowerLeftMs = maxPowerMillisecondsLeft();
  const TickType_t inputTimeoutTicks = inputWaitTicksForMaxPowerWindow(maxPowerLeftMs);

  if (!consumeInputEvents(press, inputTimeoutTicks)) {
    return;
  }
  armMaxPowerIfConfigured(kButtonMaxPowerMs);
  logPrintf("Got ButtonPress kind=%d\n", static_cast<int>(press.kind));

  if (!appIsSleeping() && press.kind == ButtonPressKind::Power) {
    enterDeepSleepAfterScreenRender();
    return;
  }

  const PageButtonResult result = press.kind == ButtonPressKind::Touch
                                      ? handlePageTouch(press.touchX, press.touchY)
                                      : handlePageButton(press.kind == ButtonPressKind::Directory
                                                             ? ButtonPressKind::Back
                                                             : press.kind);
  logPrintf("Selected page: %s\n", pageName(result.page));

  if (!result.renderRequired) {
    logPrintf("No render queued for unchanged button state.\n");
    return;
  }

  const RenderKind renderKind = result.overlayOnly ? RenderKind::DirectoryOverlay : RenderKind::ActivePage;
  const uint32_t seq = requestRender(renderKind, refreshModeFromSettings(copySettings()));
  logPrintf("Queued %s render request %lu\n", result.overlayOnly ? "directory overlay" : "active page",
            static_cast<unsigned long>(seq));
}

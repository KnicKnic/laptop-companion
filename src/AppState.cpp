#include "AppState.h"

#include <InputManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

SemaphoreHandle_t appStateMutex = nullptr;
AppState appState;

void lockAppState() {
  xSemaphoreTake(appStateMutex, portMAX_DELAY);
}

void unlockAppState() {
  xSemaphoreGive(appStateMutex);
}

}  // namespace

bool beginAppState() {
  appStateMutex = xSemaphoreCreateMutex();
  return appStateMutex != nullptr;
}

AppState copyAppState() {
  lockAppState();
  AppState copy = appState;
  unlockAppState();
  return copy;
}

void setSdReady(bool ready) {
  lockAppState();
  appState.sdReady = ready;
  unlockAppState();
}

void setCompanionSettingsStatus(bool checked, bool present, bool valid) {
  lockAppState();
  appState.companionSettingsChecked = checked;
  appState.companionSettingsPresent = present;
  appState.companionSettingsValid = valid;
  unlockAppState();
}

void setBootBatteryPercentage(bool known, uint16_t percentage) {
  lockAppState();
  appState.batteryPercentageKnown = known;
  appState.bootBatteryPercentage = percentage > 100 ? 100 : percentage;
  unlockAppState();
}

bool appIsSleeping() {
  lockAppState();
  const bool sleeping = appState.sleeping;
  unlockAppState();
  return sleeping;
}

void setAppSleeping(bool sleeping) {
  lockAppState();
  appState.sleeping = sleeping;
  unlockAppState();
}

void recordPowerInterrupt() {
  lockAppState();
  ++appState.powerInterruptCount;
  unlockAppState();
}

void recordGpio1Down() {
  lockAppState();
  ++appState.gpio1DownCount;
  ++appState.totalButtonPressCount;
  unlockAppState();
}

void recordGpio2Down() {
  lockAppState();
  ++appState.gpio2DownCount;
  ++appState.totalButtonPressCount;
  unlockAppState();
}

void recordGpio1ButtonPress(int button) {
  lockAppState();
  if (button == InputManager::BTN_BACK) {
    ++appState.backPressCount;
  } else if (button == InputManager::BTN_CONFIRM) {
    ++appState.confirmPressCount;
  } else if (button == InputManager::BTN_LEFT) {
    ++appState.leftPressCount;
  } else {
    ++appState.rightPressCount;
  }
  ++appState.totalButtonPressCount;
  unlockAppState();
}

void recordGpio2ButtonPress(int button) {
  lockAppState();
  if (button == InputManager::BTN_UP) {
    ++appState.upPressCount;
  } else {
    ++appState.downPressCount;
  }
  ++appState.totalButtonPressCount;
  unlockAppState();
}

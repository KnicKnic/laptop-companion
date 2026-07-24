#pragma once

#include <Arduino.h>

struct AppState {
  bool sleeping = false;
  bool sdReady = false;
  bool companionSettingsChecked = false;
  bool companionSettingsPresent = false;
  bool companionSettingsValid = false;
  bool batteryPercentageKnown = false;
  uint16_t bootBatteryPercentage = 0;
  uint32_t backPressCount = 0;
  uint32_t confirmPressCount = 0;
  uint32_t leftPressCount = 0;
  uint32_t rightPressCount = 0;
  uint32_t upPressCount = 0;
  uint32_t downPressCount = 0;
  uint32_t gpio1DownCount = 0;
  uint32_t gpio2DownCount = 0;
  uint32_t powerInterruptCount = 0;
  uint32_t totalButtonPressCount = 0;
};

bool beginAppState();
AppState copyAppState();

void setSdReady(bool ready);
void setCompanionSettingsStatus(bool checked, bool present, bool valid);
void setBootBatteryPercentage(bool known, uint16_t percentage);
bool appIsSleeping();
void setAppSleeping(bool sleeping);

void recordPowerInterrupt();
void recordGpio1Down();
void recordGpio2Down();
void recordGpio1ButtonPress(int button);
void recordGpio2ButtonPress(int button);

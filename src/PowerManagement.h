#pragma once

#include <esp_err.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

struct PowerManagementState {
  bool enabledBySettings = false;
  bool configured = false;
  bool autoLightSleep = false;
  esp_err_t configResult = ESP_OK;
  int maxFreqMhz = 0;
  int minFreqMhz = 0;
};

class TimedPowerManagementLock {
 public:
  TimedPowerManagementLock(esp_pm_lock_type_t type, const char* name);
  ~TimedPowerManagementLock();

  TimedPowerManagementLock(const TimedPowerManagementLock&) = delete;
  TimedPowerManagementLock& operator=(const TimedPowerManagementLock&) = delete;

  bool ensureArmed(uint32_t milliseconds);
  bool release();
  bool releaseIfExpired();
  uint32_t millisecondsLeft();
  bool armed() const { return acquired_; }

 private:
  bool ensureCreated();
  static uint64_t nowMs();

  esp_pm_lock_type_t type_;
  const char* name_;
  esp_pm_lock_handle_t handle_ = nullptr;
  uint64_t deadlineMs_ = 0;
  bool acquired_ = false;
};

bool configurePowerManagementFromSettings();
PowerManagementState copyPowerManagementState();
TimedPowerManagementLock& maxPowerLock();

// Blocks auto light sleep for a fixed window after boot so the USB-Serial-JTAG
// port (which does not survive S3 light sleep) is always usable for flashing
// and logs right after a reset, whatever the saved settings say.
void holdLightSleepOffAfterBoot();
bool lightSleepBootGraceActive();

// Dead-man switch for light sleep enabled over BLE: unless re-armed (keepalive)
// within `milliseconds`, an esp_timer (which also wakes the chip from light
// sleep) switches auto light sleep back off, independent of the BLE stack.
void armLightSleepDeadman(uint32_t milliseconds);
void disarmLightSleepDeadman();
uint32_t lightSleepDeadmanTrips();

// Live (unsaved) experiment knobs, driven over BLE by ble_sleep_soak.
// Takes effect on the next configurePowerManagementFromSettings().
// Each field: -1 = use the saved setting, 0 = force off, 1 = force on.
struct PowerManagementOverrides {
  int8_t enable = -1;
  int8_t autoLightSleep = -1;
  int8_t btWakeAutotune = -1;
  int8_t rtcCalFilter = -1;
};
void setPowerManagementOverrides(const PowerManagementOverrides& overrides);
PowerManagementOverrides powerManagementOverrides();
// Also disarms the light-sleep dead-man timer. Called when the user changes
// settings on the device, so on-device choices always win.
void clearPowerManagementOverrides();
void setPowerManagementMinFreqMhz(int mhz);
int powerManagementMinFreqMhz();
void setPmStatsDumpEnabled(bool enabled);
bool pmStatsDumpIsEnabled();
// The dump task blocks indefinitely while disabled; enabling wakes it.
void setPmStatsDumpTask(TaskHandle_t task);

#pragma once

#include <esp_err.h>
#include <esp_pm.h>

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

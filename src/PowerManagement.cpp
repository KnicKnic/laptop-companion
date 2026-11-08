#include "PowerManagement.h"

#include "AppLog.h"
#include "BtSleepTiming.h"
#include "Settings.h"

#include <BoardConfig.h>
#include <esp_pm.h>
#include <esp_timer.h>

namespace {

constexpr int kMaxCpuMhz = 240;
constexpr int kMinCpuMhz = 10;
constexpr uint64_t kBootNoLightSleepUs = 60ULL * 1000ULL * 1000ULL;

PowerManagementState state;
TimedPowerManagementLock maxPower(ESP_PM_CPU_FREQ_MAX, "max-power");
esp_pm_lock_handle_t bootNoLightSleepLock = nullptr;
esp_timer_handle_t bootNoLightSleepTimer = nullptr;
volatile bool bootGraceActive = false;

void releaseBootNoLightSleep(void*) {
  if (bootNoLightSleepLock && bootGraceActive) {
    esp_pm_lock_release(bootNoLightSleepLock);
  }
  bootGraceActive = false;
  logPrintf("Boot light-sleep grace window ended; auto light sleep may now disable USB.\n");
}

}  // namespace

TimedPowerManagementLock::TimedPowerManagementLock(esp_pm_lock_type_t type, const char* name)
    : type_(type), name_(name) {}

TimedPowerManagementLock::~TimedPowerManagementLock() {
  if (acquired_ && handle_) {
    esp_pm_lock_release(handle_);
  }
  if (handle_) {
    esp_pm_lock_delete(handle_);
  }
}

bool TimedPowerManagementLock::ensureArmed(uint32_t milliseconds) {
  releaseIfExpired();
  if (milliseconds == 0) return true;
  if (!ensureCreated()) return false;

  const uint64_t requestedDeadline = nowMs() + milliseconds;
  if (!acquired_) {
    const esp_err_t err = esp_pm_lock_acquire(handle_);
    if (err != ESP_OK) {
      logPrintf("Power lock %s acquire failed: %s\n", name_, esp_err_to_name(err));
      return false;
    }
    acquired_ = true;
  }

  if (requestedDeadline > deadlineMs_) {
    deadlineMs_ = requestedDeadline;
  }
  return true;
}

bool TimedPowerManagementLock::releaseIfExpired() {
  if (!acquired_) return false;
  if (nowMs() < deadlineMs_) return false;

  return release();
}

bool TimedPowerManagementLock::release() {
  if (!acquired_) return false;

  const esp_err_t err = esp_pm_lock_release(handle_);
  if (err != ESP_OK) {
    logPrintf("Power lock %s release failed: %s\n", name_, esp_err_to_name(err));
    return false;
  }

  acquired_ = false;
  deadlineMs_ = 0;
  return true;
}

uint32_t TimedPowerManagementLock::millisecondsLeft() {
  releaseIfExpired();
  if (!acquired_) return 0;

  const uint64_t now = nowMs();
  if (now >= deadlineMs_) {
    releaseIfExpired();
    return 0;
  }

  const uint64_t left = deadlineMs_ - now;
  return left > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(left);
}

bool TimedPowerManagementLock::ensureCreated() {
  if (handle_) return true;

  const esp_err_t err = esp_pm_lock_create(type_, 0, name_, &handle_);
  if (err != ESP_OK) {
    logPrintf("Power lock %s create failed: %s\n", name_, esp_err_to_name(err));
    handle_ = nullptr;
    return false;
  }
  return true;
}

uint64_t TimedPowerManagementLock::nowMs() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000LL);
}

bool configurePowerManagementFromSettings() {
  const CompanionSettings settings = copySettings();
  setBtWakeAutotuneEnabled(settings.system.powerManagement.btWakeAutotune);
  setRtcCalFilterEnabled(settings.system.powerManagement.rtcCalFilter);
  state.enabledBySettings = settings.system.powerManagement.enable;
  state.configured = false;
  state.autoLightSleep = settings.system.powerManagement.autoLightSleep;
  state.configResult = ESP_OK;
  state.maxFreqMhz = kMaxCpuMhz;
  state.minFreqMhz = kMinCpuMhz;

  if (!settings.system.powerManagement.enable) {
    logPrintf("Power management disabled by settings.\n");
    return true;
  }

  esp_pm_config_t pmConfig = {};
  pmConfig.max_freq_mhz = kMaxCpuMhz;
  pmConfig.min_freq_mhz = kMinCpuMhz;
  pmConfig.light_sleep_enable = settings.system.powerManagement.autoLightSleep;
  if (pmConfig.light_sleep_enable) {
    BoardConfig::holdPowerRailsForLightSleep();
  }

  const esp_err_t configResult = esp_pm_configure(&pmConfig);

  esp_pm_config_t activeConfig = {};
  const esp_err_t getConfigResult = esp_pm_get_configuration(&activeConfig);

  state.configResult = configResult;
  state.configured = configResult == ESP_OK;
  if (getConfigResult == ESP_OK) {
    state.maxFreqMhz = activeConfig.max_freq_mhz;
    state.minFreqMhz = activeConfig.min_freq_mhz;
    state.autoLightSleep = activeConfig.light_sleep_enable;
  } else {
    state.maxFreqMhz = pmConfig.max_freq_mhz;
    state.minFreqMhz = pmConfig.min_freq_mhz;
    state.autoLightSleep = pmConfig.light_sleep_enable && configResult == ESP_OK;
  }

  logPrintf("Power management: %s, DFS %d->%d MHz, auto light sleep %s, bt wake autotune %s, rtc cal filter %s\n",
            configResult == ESP_OK ? "enabled" : esp_err_to_name(configResult), state.maxFreqMhz, state.minFreqMhz,
            state.autoLightSleep ? "on" : "off", settings.system.powerManagement.btWakeAutotune ? "on" : "off",
            settings.system.powerManagement.rtcCalFilter ? "on" : "off");
  return configResult == ESP_OK;
}

PowerManagementState copyPowerManagementState() {
  return state;
}

TimedPowerManagementLock& maxPowerLock() {
  return maxPower;
}

void holdLightSleepOffAfterBoot() {
  if (bootNoLightSleepLock) return;
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "boot-usb", &bootNoLightSleepLock) != ESP_OK ||
      esp_pm_lock_acquire(bootNoLightSleepLock) != ESP_OK) {
    logPrintf("Boot light-sleep grace lock failed\n");
    return;
  }
  bootGraceActive = true;
  const esp_timer_create_args_t args = {
      .callback = releaseBootNoLightSleep,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "boot-usb",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &bootNoLightSleepTimer) != ESP_OK ||
      esp_timer_start_once(bootNoLightSleepTimer, kBootNoLightSleepUs) != ESP_OK) {
    // Never leave the lock held forever because a timer failed.
    releaseBootNoLightSleep(nullptr);
    return;
  }
  logPrintf("Auto light sleep held off for %llus after boot (USB stays usable).\n",
            static_cast<unsigned long long>(kBootNoLightSleepUs / 1000000ULL));
}

bool lightSleepBootGraceActive() {
  return bootGraceActive;
}

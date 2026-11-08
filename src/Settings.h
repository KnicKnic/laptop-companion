#pragma once

#include <Arduino.h>
#include <EInkDisplay.h>

enum class SettingsLoadResult : uint8_t {
  NotLoaded,
  Loaded,
  SdUnavailable,
  Missing,
  InvalidJson,
  SaveFailed,
};

struct CompanionSettings {
  struct PowerManagement {
    bool enable = false;
    bool autoLightSleep = false;
    // Auto-tune the BLE controller's light-sleep wake lead (see BtSleepTiming.h).
    bool btWakeAutotune = false;
    // Use the periodic 1024-cycle RTC slow clock reference calibration instead of
    // IDF's 10-cycle per-light-sleep one.
    bool rtcCalFilter = false;
  };

  struct Page {
    String startup = "/companion";
  };

  struct Display {
    String refreshMode = "fast";
  };

  struct Sleep {
    String imagePath = "/sleep/sleep_screen.bmp";
  };

  struct Buttons {
    String gpio2DownAction = "";
    String gpio1UpAction = "";
  };

  struct Debug {
    bool serialLogging = true;
    bool showPowerStatsPage = true;
  };

  // Frontlight test/feedback levels: also used by the touch driver as its
  // touch-down flash brightness/color (0 brightness effectively disables the
  // flash). Board-agnostic; a no-op on boards without a frontlight.
  struct Frontlight {
    uint8_t touchBrightness = 60;
    uint8_t touchColorTemperature = 50;
  };

  // GT911 touch-key sensitivity (all four virtual key slots, including the
  // Home key), as a single 0-15 nibble applied to both sensitivity registers.
  // Only meaningful on input backends that report
  // inputSupportsHomeKeySensitivity() (currently X4 Pro); a no-op elsewhere.
  struct Touch {
    uint8_t homeKeySensitivity = 9;  // matches the GT911 config the SDK shipped with
  };

  struct System {
    PowerManagement powerManagement;
    Page page;
    Display display;
    Sleep sleep;
    Buttons buttons;
    Debug debug;
    Frontlight frontlight;
    Touch touch;
  };

  System system;

  void resetToDefaults();
  bool deserialize(const String& json, char* error, size_t errorLen);
  String serialize() const;
};

constexpr const char* kCompanionSettingsPath = "/companion_settings.json";

bool beginSettings();
SettingsLoadResult loadSettingsFromSd(const char* path = kCompanionSettingsPath);
bool saveSettingsToSd(const char* path = kCompanionSettingsPath);
CompanionSettings copySettings();
void updateSettings(const CompanionSettings& settings);
SettingsLoadResult lastSettingsLoadResult();
const char* settingsLoadResultName(SettingsLoadResult result);
EInkDisplay::RefreshMode refreshModeFromSettings(const CompanionSettings& settings);

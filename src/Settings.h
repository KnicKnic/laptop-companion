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

  struct System {
    PowerManagement powerManagement;
    Page page;
    Display display;
    Sleep sleep;
    Buttons buttons;
    Debug debug;
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

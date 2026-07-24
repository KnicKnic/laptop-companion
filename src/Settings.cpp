#include "Settings.h"

#include "AppLog.h"

#include <SDCardManager.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace {

SemaphoreHandle_t settingsMutex = nullptr;
CompanionSettings currentSettings;
SettingsLoadResult loadResult = SettingsLoadResult::NotLoaded;

bool isStartupPageName(const char* value) {
  return std::strcmp(value, "/") == 0 || std::strcmp(value, "/companion") == 0 ||
         std::strcmp(value, "/companion/stats") == 0 || std::strcmp(value, "/settings") == 0 ||
         std::strcmp(value, "/other/test") == 0 || std::strcmp(value, "/other/power-stats") == 0 ||
         std::strcmp(value, "/other/error") == 0 || std::strcmp(value, "main") == 0 ||
         std::strcmp(value, "companion") == 0 || std::strcmp(value, "power_stats") == 0 ||
         std::strcmp(value, "settings") == 0;
}

bool isRefreshModeName(const char* value) {
  return std::strcmp(value, "fast") == 0 || std::strcmp(value, "full") == 0 ||
         std::strcmp(value, "half") == 0;
}

void setError(char* error, size_t errorLen, const char* message) {
  if (!error || errorLen == 0) return;
  snprintf(error, errorLen, "%s", message);
}

const cJSON* objectItem(const cJSON* object, const char* key) {
  if (!cJSON_IsObject(object)) return nullptr;
  return cJSON_GetObjectItemCaseSensitive(object, key);
}

bool jsonBoolOrDefault(const cJSON* object, const char* key, bool defaultValue) {
  const cJSON* item = objectItem(object, key);
  if (!cJSON_IsBool(item)) return defaultValue;
  return cJSON_IsTrue(item);
}

String jsonStringOrDefault(const cJSON* object, const char* key, const String& defaultValue) {
  const cJSON* item = objectItem(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) return defaultValue;
  return String(item->valuestring);
}

cJSON* addObject(cJSON* parent, const char* key) {
  cJSON* object = cJSON_CreateObject();
  cJSON_AddItemToObject(parent, key, object);
  return object;
}

}  // namespace

void CompanionSettings::resetToDefaults() {
  *this = CompanionSettings{};
}

bool CompanionSettings::deserialize(const String& json, char* error, size_t errorLen) {
  CompanionSettings parsed;

  cJSON* root = cJSON_Parse(json.c_str());
  if (root == nullptr) {
    setError(error, errorLen, "parse failed");
    return false;
  }

  const cJSON* systemObj = objectItem(root, "system");
  if (!cJSON_IsObject(systemObj)) {
    setError(error, errorLen, "missing system object");
    cJSON_Delete(root);
    return false;
  }

  const cJSON* power = objectItem(systemObj, "power_management");
  parsed.system.powerManagement.enable =
      jsonBoolOrDefault(power, "enable", parsed.system.powerManagement.enable);
  parsed.system.powerManagement.autoLightSleep =
      jsonBoolOrDefault(power, "auto_light_sleep", parsed.system.powerManagement.autoLightSleep);

  const cJSON* page = objectItem(systemObj, "page");
  const String startup = jsonStringOrDefault(page, "startup", parsed.system.page.startup);
  if (isStartupPageName(startup.c_str())) {
    parsed.system.page.startup = startup;
  }

  const cJSON* display = objectItem(systemObj, "display");
  const String refreshMode = jsonStringOrDefault(display, "refresh_mode", parsed.system.display.refreshMode);
  if (isRefreshModeName(refreshMode.c_str())) {
    parsed.system.display.refreshMode = refreshMode;
  }

  const cJSON* sleep = objectItem(systemObj, "sleep");
  parsed.system.sleep.imagePath = jsonStringOrDefault(sleep, "image_path", parsed.system.sleep.imagePath);

  const cJSON* buttons = objectItem(systemObj, "buttons");
  parsed.system.buttons.gpio2DownAction = jsonStringOrDefault(buttons, "gpio2_down_action", "");
  parsed.system.buttons.gpio1UpAction = jsonStringOrDefault(buttons, "gpio1_up_action", "");

  const cJSON* debug = objectItem(systemObj, "debug");
  parsed.system.debug.serialLogging = jsonBoolOrDefault(debug, "serial_logging", parsed.system.debug.serialLogging);
  parsed.system.debug.showPowerStatsPage =
      jsonBoolOrDefault(debug, "show_power_stats_page", parsed.system.debug.showPowerStatsPage);

  *this = parsed;
  setError(error, errorLen, "");
  cJSON_Delete(root);
  return true;
}

String CompanionSettings::serialize() const {
  cJSON* root = cJSON_CreateObject();
  cJSON* systemObj = addObject(root, "system");

  cJSON* power = addObject(systemObj, "power_management");
  cJSON_AddBoolToObject(power, "enable", system.powerManagement.enable);
  cJSON_AddBoolToObject(power, "auto_light_sleep", system.powerManagement.autoLightSleep);

  cJSON* page = addObject(systemObj, "page");
  cJSON_AddStringToObject(page, "startup", system.page.startup.c_str());

  cJSON* display = addObject(systemObj, "display");
  cJSON_AddStringToObject(display, "refresh_mode", system.display.refreshMode.c_str());

  cJSON* sleep = addObject(systemObj, "sleep");
  cJSON_AddStringToObject(sleep, "image_path", system.sleep.imagePath.c_str());

  cJSON* buttons = addObject(systemObj, "buttons");
  cJSON_AddStringToObject(buttons, "gpio2_down_action", system.buttons.gpio2DownAction.c_str());
  cJSON_AddStringToObject(buttons, "gpio1_up_action", system.buttons.gpio1UpAction.c_str());

  cJSON* debug = addObject(systemObj, "debug");
  cJSON_AddBoolToObject(debug, "serial_logging", system.debug.serialLogging);
  cJSON_AddBoolToObject(debug, "show_power_stats_page", system.debug.showPowerStatsPage);

  char* printed = cJSON_Print(root);
  String out = printed ? String(printed) : String("{}");
  out += "\n";
  cJSON_free(printed);
  cJSON_Delete(root);
  return out;
}

bool beginSettings() {
  settingsMutex = xSemaphoreCreateMutex();
  return settingsMutex != nullptr;
}

SettingsLoadResult loadSettingsFromSd(const char* path) {
  CompanionSettings loaded;
  loaded.resetToDefaults();

  if (!SdMan.ready()) {
    xSemaphoreTake(settingsMutex, portMAX_DELAY);
    currentSettings = loaded;
    loadResult = SettingsLoadResult::SdUnavailable;
    xSemaphoreGive(settingsMutex);
    return SettingsLoadResult::SdUnavailable;
  }

  if (!SdMan.exists(path)) {
    xSemaphoreTake(settingsMutex, portMAX_DELAY);
    currentSettings = loaded;
    loadResult = SettingsLoadResult::Missing;
    xSemaphoreGive(settingsMutex);
    return SettingsLoadResult::Missing;
  }

  const String json = SdMan.readFile(path);
  char error[80] = {};
  if (!loaded.deserialize(json, error, sizeof(error))) {
    logPrintf("Settings parse failed for %s: %s\n", path, error);
    xSemaphoreTake(settingsMutex, portMAX_DELAY);
    currentSettings.resetToDefaults();
    loadResult = SettingsLoadResult::InvalidJson;
    xSemaphoreGive(settingsMutex);
    return SettingsLoadResult::InvalidJson;
  }

  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  currentSettings = loaded;
  loadResult = SettingsLoadResult::Loaded;
  xSemaphoreGive(settingsMutex);
  return SettingsLoadResult::Loaded;
}

bool saveSettingsToSd(const char* path) {
  if (!SdMan.ready()) {
    xSemaphoreTake(settingsMutex, portMAX_DELAY);
    loadResult = SettingsLoadResult::SdUnavailable;
    xSemaphoreGive(settingsMutex);
    return false;
  }

  const CompanionSettings settings = copySettings();
  const bool saved = SdMan.writeFile(path, settings.serialize());
  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  loadResult = saved ? SettingsLoadResult::Loaded : SettingsLoadResult::SaveFailed;
  xSemaphoreGive(settingsMutex);
  logPrintf("Settings save %s: %s\n", path, saved ? "ok" : "failed");
  return saved;
}

CompanionSettings copySettings() {
  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  CompanionSettings copy = currentSettings;
  xSemaphoreGive(settingsMutex);
  return copy;
}

void updateSettings(const CompanionSettings& settings) {
  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  currentSettings = settings;
  xSemaphoreGive(settingsMutex);
}

SettingsLoadResult lastSettingsLoadResult() {
  xSemaphoreTake(settingsMutex, portMAX_DELAY);
  const SettingsLoadResult result = loadResult;
  xSemaphoreGive(settingsMutex);
  return result;
}

const char* settingsLoadResultName(SettingsLoadResult result) {
  switch (result) {
    case SettingsLoadResult::NotLoaded:
      return "not loaded";
    case SettingsLoadResult::Loaded:
      return "loaded";
    case SettingsLoadResult::SdUnavailable:
      return "SD unavailable";
    case SettingsLoadResult::Missing:
      return "missing";
    case SettingsLoadResult::InvalidJson:
      return "invalid JSON";
    case SettingsLoadResult::SaveFailed:
      return "save failed";
  }
  return "unknown";
}

EInkDisplay::RefreshMode refreshModeFromSettings(const CompanionSettings& settings) {
  if (settings.system.display.refreshMode == "full") return EInkDisplay::FULL_REFRESH;
  if (settings.system.display.refreshMode == "half") return EInkDisplay::HALF_REFRESH;
  return EInkDisplay::FAST_REFRESH;
}

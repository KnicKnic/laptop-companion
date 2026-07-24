#include "SettingsPage.h"

#include "AppLog.h"
#include "AppState.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "Settings.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

SettingsPage::SettingsPage(EInkDisplay& display) : Page(display) {}

PageId SettingsPage::id() const {
  return PageId::Settings;
}

const char* SettingsPage::name() const {
  return "settings";
}

bool SettingsPage::handleButton(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Confirm:
      toggleSelectedSetting();
      return true;
    case ButtonPressKind::Right:
      selectedRow_ = (selectedRow_ + 1) % kEditableRows;
      return true;
    case ButtonPressKind::Left:
      selectedRow_ = (selectedRow_ + kEditableRows - 1) % kEditableRows;
      return true;
    default:
      return false;
  }
}

void SettingsPage::onLeave() {
  if (!dirty_) return;
  if (saveSettingsToSd()) {
    setCompanionSettingsStatus(true, true, true);
    dirty_ = false;
  }
}

std::unique_ptr<RenderTransaction> SettingsPage::render(freeink::ui::DisplayTarget& target,
                                                        EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  const CompanionSettings settings = copySettings();

  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 2;

  char titleLine[64];
  snprintf(titleLine, sizeof(titleLine), "Settings%s", dirty_ ? " *" : "");

  char pmEnable[16];
  char autoSleep[16];
  char logging[16];
  char powerStats[16];
  snprintf(pmEnable, sizeof(pmEnable), "%s", boolText(settings.system.powerManagement.enable));
  snprintf(autoSleep, sizeof(autoSleep), "%s", boolText(settings.system.powerManagement.autoLightSleep));
  snprintf(logging, sizeof(logging), "%s", boolText(settings.system.debug.serialLogging));
  snprintf(powerStats, sizeof(powerStats), "%s", boolText(settings.system.debug.showPowerStatsPage));

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 20), content.width, 44},
                        titleLine, title);

  const int16_t rowX = static_cast<int16_t>(content.x + 20);
  const int16_t rowW = static_cast<int16_t>(content.width - 40);
  constexpr int16_t rowH = 44;
  int16_t y = static_cast<int16_t>(content.y + 96);
  drawSettingRow(target, freeink::ui::Rect{rowX, y, rowW, rowH}, "Power management", pmEnable, selectedRow_ == 0);
  y += 50;
  drawSettingRow(target, freeink::ui::Rect{rowX, y, rowW, rowH}, "Auto light sleep", autoSleep, selectedRow_ == 1);
  y += 50;
  drawSettingRow(target, freeink::ui::Rect{rowX, y, rowW, rowH}, "Startup page", settings.system.page.startup.c_str(),
                 selectedRow_ == 2);
  y += 50;
  drawSettingRow(target, freeink::ui::Rect{rowX, y, rowW, rowH}, "Refresh mode",
                 settings.system.display.refreshMode.c_str(), selectedRow_ == 3);
  y += 50;
  drawSettingRow(target, freeink::ui::Rect{rowX, y, rowW, rowH}, "Serial logging", logging, selectedRow_ == 4);
  y += 50;
  drawSettingRow(target, freeink::ui::Rect{rowX, y, rowW, rowH}, "Power stats page", powerStats, selectedRow_ == 5);

  char sleepPathLine[96];
  snprintf(sleepPathLine, sizeof(sleepPathLine), "Sleep image: %s", settings.system.sleep.imagePath.c_str());
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 86), content.width, 30},
                        sleepPathLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 42), content.width, 30},
                        "Left/Right: select setting   Confirm: toggle", body);
  drawPageChrome(target);
  return tx;
}

void SettingsPage::toggleSelectedSetting() {
  CompanionSettings settings = copySettings();
  switch (selectedRow_) {
    case 0:
      settings.system.powerManagement.enable = !settings.system.powerManagement.enable;
      break;
    case 1:
      settings.system.powerManagement.autoLightSleep = !settings.system.powerManagement.autoLightSleep;
      break;
    case 2:
      settings.system.page.startup = nextStartupPage(settings.system.page.startup);
      break;
    case 3:
      settings.system.display.refreshMode = nextRefreshMode(settings.system.display.refreshMode);
      break;
    case 4:
      settings.system.debug.serialLogging = !settings.system.debug.serialLogging;
      setSerialLoggingEnabled(settings.system.debug.serialLogging);
      break;
    case 5:
      settings.system.debug.showPowerStatsPage = !settings.system.debug.showPowerStatsPage;
      break;
  }
  updateSettings(settings);
  dirty_ = true;
}

String SettingsPage::nextStartupPage(const String& current) {
  if (current == "/companion") return "/settings";
  if (current == "/settings") return "/other/test";
  if (current == "/other/test") return "/other/power-stats";
  if (current == "/other/power-stats") return "/other/error";
  if (current == "/other/error") return "/";
  return "/companion";
}

String SettingsPage::nextRefreshMode(const String& current) {
  if (current == "fast") return "half";
  if (current == "half") return "full";
  return "fast";
}

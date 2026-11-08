#include "SettingsPage.h"

#include "AppLog.h"
#include "AppState.h"
#include "Frontlight.h"
#include "IsrInput.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "PowerManagement.h"
#include "Settings.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

namespace {
constexpr freeink::ui::ActionId kActionChangeSetting = 1;
constexpr freeink::ui::ActionId kActionFrontlightBrightnessSlider = 2;
constexpr freeink::ui::ActionId kActionFrontlightColorSlider = 3;
constexpr int16_t kRowH = 44;
constexpr int16_t kRowStep = 50;
// Width of the label portion of a frontlight row; the slider track fills the
// rest. The percentage is folded into the label text itself (see
// SettingsPage::render), so there's no separate value column to collide with.
constexpr int16_t kFrontlightLabelWidth = 230;

// Row geometry shared by render() and handleTouch() so a tap always lands on
// exactly the row it visually overlaps. `position` is the row's 0-based slot
// among the currently active rows (SettingsPage::activeRows), not a row id.
freeink::ui::Rect settingsRowRect(const freeink::ui::Rect& content, uint8_t position) {
  const int16_t rowX = static_cast<int16_t>(content.x + 20);
  const int16_t rowW = static_cast<int16_t>(content.width - 40);
  const int16_t firstRowY = static_cast<int16_t>(content.y + 96);
  return freeink::ui::Rect{rowX, static_cast<int16_t>(firstRowY + position * kRowStep), rowW, kRowH};
}

freeink::ui::Rect frontlightSliderTrack(const freeink::ui::Rect& row) {
  return freeink::ui::Rect{static_cast<int16_t>(row.x + kFrontlightLabelWidth), row.y,
                           static_cast<int16_t>(row.width - kFrontlightLabelWidth), row.height};
}

void drawFrontlightSlider(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& row, uint8_t percent) {
  // A throwaway Frame purely for drawing: hit-testing for the slider happens
  // separately in handleTouch() against the same frontlightSliderTrack() rect.
  freeink::ui::InputSnapshot input;
  freeink::ui::InteractionBuffer<1> interactions;
  freeink::ui::Frame<1> frame(target, target.deviceContext(), input, interactions);
  freeink::ui::SliderProps props;
  props.value = percent;
  props.max = 100;
  props.trackHeight = 8;
  props.knobWidth = 12;
  props.knobHeight = static_cast<int16_t>(row.height - 14);
  props.horizontalPadding = 6;
  freeink::ui::slider(frame, frontlightSliderTrack(row), props);
}

// Steps a 0-100 percentage up by 10, wrapping back to 0 past 100. Used by
// Confirm/tap-on-label so the frontlight rows follow the same "Confirm
// cycles the value" model as every other row on this page.
uint8_t stepPercent(uint8_t value) {
  return static_cast<uint8_t>(((value / 10) + 1) % 11 * 10);
}

// Steps a 0-15 nibble up by 1, wrapping back to 0 past 15 (0->f, then back to
// 0), for the GT911 home-key sensitivity row.
uint8_t stepNibble(uint8_t value) {
  return static_cast<uint8_t>((value + 1) % 16);
}
}  // namespace

SettingsPage::SettingsPage(EInkDisplay& display) : Page(display) {}

PageId SettingsPage::id() const {
  return PageId::Settings;
}

const char* SettingsPage::name() const {
  return "settings";
}

uint8_t SettingsPage::activeRows(uint8_t* rows) {
  uint8_t count = 0;
  rows[count++] = kRowPowerManagement;
  rows[count++] = kRowAutoLightSleep;
  rows[count++] = kRowBtWakeAutotune;
  rows[count++] = kRowRtcCalFilter;
  rows[count++] = kRowStartupPage;
  rows[count++] = kRowRefreshMode;
  rows[count++] = kRowSerialLogging;
  rows[count++] = kRowPowerStatsPage;
  if (frontlightPresent()) {
    rows[count++] = kRowFrontlightBrightness;
    if (frontlightHasColorTemperature()) rows[count++] = kRowFrontlightColor;
  }
  if (inputSupportsHomeKeySensitivity()) rows[count++] = kRowHomeKeySensitivity;
  return count;
}

void SettingsPage::moveSelection(int8_t direction) {
  uint8_t rows[kMaxRows];
  const uint8_t count = activeRows(rows);
  if (count == 0) return;

  int position = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (rows[i] == selectedRow_) {
      position = i;
      break;
    }
  }
  position = (position + direction + count) % count;
  selectedRow_ = rows[position];
}

bool SettingsPage::handleButton(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Confirm:
      toggleSelectedSetting();
      return true;
    case ButtonPressKind::Right:
      moveSelection(1);
      return true;
    case ButtonPressKind::Left:
      moveSelection(-1);
      return true;
    default:
      return false;
  }
}

bool SettingsPage::handleTouch(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) {
  // Keep the tap geometry identical to the rendered setting rows. A tap
  // changes that setting immediately; it no longer acts through a page-wide
  // left/middle/right zone.
  const freeink::ui::Rect content = pageMainPanel(target).inset(freeink::ui::Insets{12, 8, 24, 18});

  uint8_t rows[kMaxRows];
  const uint8_t count = activeRows(rows);

  freeink::ui::InputSnapshot input;
  input.touchReleased = true;
  input.touchX = x;
  input.touchY = y;
  freeink::ui::InteractionBuffer<kMaxRows + 2> interactions;
  freeink::ui::Frame<kMaxRows + 2> frame(target, target.deviceContext(), input, interactions);

  // Whole-row hits first (Confirm-equivalent: cycles the value), then the
  // frontlight slider sub-rects on top of them. Frame resolves overlapping
  // hits in most-recently-added-first order, so a tap on the slider itself
  // is claimed by the slider hit even though the row hit also covers it.
  for (uint8_t position = 0; position < count; ++position) {
    frame.hit(settingsRowRect(content, position), kActionChangeSetting, rows[position], freeink::ui::InputTouch);
  }
  for (uint8_t position = 0; position < count; ++position) {
    if (rows[position] == kRowFrontlightBrightness) {
      frame.hit(frontlightSliderTrack(settingsRowRect(content, position)), kActionFrontlightBrightnessSlider, 0,
                freeink::ui::InputTouch | freeink::ui::InputDrag);
    } else if (rows[position] == kRowFrontlightColor) {
      frame.hit(frontlightSliderTrack(settingsRowRect(content, position)), kActionFrontlightColorSlider, 0,
                freeink::ui::InputTouch | freeink::ui::InputDrag);
    }
  }

  const freeink::ui::ActionEvent event = frame.finish();
  if (event.action == kActionFrontlightBrightnessSlider && event.dragPermille >= 0) {
    setFrontlightBrightnessValue(static_cast<uint8_t>(event.dragPermille / 10));
    return true;
  }
  if (event.action == kActionFrontlightColorSlider && event.dragPermille >= 0) {
    setFrontlightColorValue(static_cast<uint8_t>(event.dragPermille / 10));
    return true;
  }
  if (event.action != kActionChangeSetting) return false;

  selectedRow_ = static_cast<uint8_t>(event.value);
  toggleSelectedSetting();
  return true;
}

void SettingsPage::onLeave() {
  // The frontlight rows drive the hardware directly so the value can be
  // previewed while adjusting it; always leave it off when navigating away
  // rather than only on a "dirty" change, since a preview may have been left
  // running without ever nudging a value (e.g. viewing then leaving).
  setFrontlightBrightness(0);

  if (!dirty_) return;
  if (saveSettingsToSd()) {
    setCompanionSettingsStatus(true, true, true);
  }
  // Re-apply the live settings now rather than waiting for a reboot: power
  // management (DFS/light-sleep) is normally configured once at boot, so a
  // change made on this page would otherwise sit unused until next boot.
  configurePowerManagementFromSettings();
  dirty_ = false;
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
  char btWakeAutotune[16];
  char rtcCalFilter[16];
  char logging[16];
  char powerStats[16];
  snprintf(pmEnable, sizeof(pmEnable), "%s", boolText(settings.system.powerManagement.enable));
  snprintf(autoSleep, sizeof(autoSleep), "%s", boolText(settings.system.powerManagement.autoLightSleep));
  snprintf(btWakeAutotune, sizeof(btWakeAutotune), "%s", boolText(settings.system.powerManagement.btWakeAutotune));
  snprintf(rtcCalFilter, sizeof(rtcCalFilter), "%s", boolText(settings.system.powerManagement.rtcCalFilter));
  snprintf(logging, sizeof(logging), "%s", boolText(settings.system.debug.serialLogging));
  snprintf(powerStats, sizeof(powerStats), "%s", boolText(settings.system.debug.showPowerStatsPage));

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 20), content.width, 44},
                        titleLine, title);

  uint8_t rows[kMaxRows];
  const uint8_t count = activeRows(rows);
  char brightnessLabel[40];
  char colorLabel[48];
  char sensitivityValue[8];
  for (uint8_t position = 0; position < count; ++position) {
    const freeink::ui::Rect row = settingsRowRect(content, position);
    const bool selected = selectedRow_ == rows[position];
    switch (rows[position]) {
      case kRowPowerManagement:
        drawSettingRow(target, row, "Power management", pmEnable, selected);
        break;
      case kRowAutoLightSleep:
        drawSettingRow(target, row, "Auto light sleep", autoSleep, selected);
        break;
      case kRowBtWakeAutotune:
        drawSettingRow(target, row, "BT wake autotune", btWakeAutotune, selected);
        break;
      case kRowRtcCalFilter:
        drawSettingRow(target, row, "RTC cal filter", rtcCalFilter, selected);
        break;
      case kRowStartupPage:
        drawSettingRow(target, row, "Startup page", settings.system.page.startup.c_str(), selected);
        break;
      case kRowRefreshMode:
        drawSettingRow(target, row, "Refresh mode", settings.system.display.refreshMode.c_str(), selected);
        break;
      case kRowSerialLogging:
        drawSettingRow(target, row, "Serial logging", logging, selected);
        break;
      case kRowPowerStatsPage:
        drawSettingRow(target, row, "Power stats page", powerStats, selected);
        break;
      case kRowFrontlightBrightness:
        snprintf(brightnessLabel, sizeof(brightnessLabel), "Touch brightness: %u%%",
                 settings.system.frontlight.touchBrightness);
        drawSettingRow(target, row, brightnessLabel, nullptr, selected);
        drawFrontlightSlider(target, row, settings.system.frontlight.touchBrightness);
        break;
      case kRowFrontlightColor:
        snprintf(colorLabel, sizeof(colorLabel), "Touch color (warm): %u%%",
                 settings.system.frontlight.touchColorTemperature);
        drawSettingRow(target, row, colorLabel, nullptr, selected);
        drawFrontlightSlider(target, row, settings.system.frontlight.touchColorTemperature);
        break;
      case kRowHomeKeySensitivity:
        snprintf(sensitivityValue, sizeof(sensitivityValue), "0x%X", settings.system.touch.homeKeySensitivity);
        drawSettingRow(target, row, "Home key sensitivity", sensitivityValue, selected);
        break;
    }
  }

  char sleepPathLine[96];
  snprintf(sleepPathLine, sizeof(sleepPathLine), "Sleep image: %s", settings.system.sleep.imagePath.c_str());
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 86), content.width, 30},
                        sleepPathLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 42), content.width, 30},
                        "Left/Right: select setting   Confirm: toggle   Tap a slider to set it", body);
  drawPageChrome(target);
  return tx;
}

void SettingsPage::toggleSelectedSetting() {
  CompanionSettings settings = copySettings();
  switch (selectedRow_) {
    case kRowPowerManagement:
      settings.system.powerManagement.enable = !settings.system.powerManagement.enable;
      break;
    case kRowAutoLightSleep:
      settings.system.powerManagement.autoLightSleep = !settings.system.powerManagement.autoLightSleep;
      break;
    case kRowBtWakeAutotune:
      settings.system.powerManagement.btWakeAutotune = !settings.system.powerManagement.btWakeAutotune;
      break;
    case kRowRtcCalFilter:
      settings.system.powerManagement.rtcCalFilter = !settings.system.powerManagement.rtcCalFilter;
      break;
    case kRowStartupPage:
      settings.system.page.startup = nextStartupPage(settings.system.page.startup);
      break;
    case kRowRefreshMode:
      settings.system.display.refreshMode = nextRefreshMode(settings.system.display.refreshMode);
      break;
    case kRowSerialLogging:
      settings.system.debug.serialLogging = !settings.system.debug.serialLogging;
      setSerialLoggingEnabled(settings.system.debug.serialLogging);
      break;
    case kRowPowerStatsPage:
      settings.system.debug.showPowerStatsPage = !settings.system.debug.showPowerStatsPage;
      break;
    case kRowFrontlightBrightness:
      settings.system.frontlight.touchBrightness = stepPercent(settings.system.frontlight.touchBrightness);
      setFrontlightBrightness(settings.system.frontlight.touchBrightness);
      break;
    case kRowFrontlightColor:
      settings.system.frontlight.touchColorTemperature = stepPercent(settings.system.frontlight.touchColorTemperature);
      setFrontlightColorTemperature(settings.system.frontlight.touchColorTemperature);
      break;
    case kRowHomeKeySensitivity:
      settings.system.touch.homeKeySensitivity = stepNibble(settings.system.touch.homeKeySensitivity);
      setHomeKeySensitivity(settings.system.touch.homeKeySensitivity);
      break;
  }
  updateSettings(settings);
  dirty_ = true;
}

void SettingsPage::setFrontlightBrightnessValue(uint8_t percent) {
  CompanionSettings settings = copySettings();
  settings.system.frontlight.touchBrightness = percent;
  updateSettings(settings);
  dirty_ = true;
  setFrontlightBrightness(percent);
}

void SettingsPage::setFrontlightColorValue(uint8_t percent) {
  CompanionSettings settings = copySettings();
  settings.system.frontlight.touchColorTemperature = percent;
  updateSettings(settings);
  dirty_ = true;
  setFrontlightColorTemperature(percent);
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


#pragma once

#include "Page.h"

class SettingsPage final : public Page {
 public:
  explicit SettingsPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  bool handleButton(ButtonPressKind kind) override;
  bool handleTouch(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) override;
  void onLeave() override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  // Row identifiers are stable regardless of which rows are actually shown,
  // so selectedRow_ keeps meaning across a change in frontlight capability.
  // The frontlight rows only appear (activeRows()) on boards that report
  // frontlightPresent()/frontlightHasColorTemperature(); the home-key
  // sensitivity row only appears when inputSupportsHomeKeySensitivity().
  static constexpr uint8_t kRowPowerManagement = 0;
  static constexpr uint8_t kRowAutoLightSleep = 1;
  static constexpr uint8_t kRowStartupPage = 2;
  static constexpr uint8_t kRowRefreshMode = 3;
  static constexpr uint8_t kRowSerialLogging = 4;
  static constexpr uint8_t kRowPowerStatsPage = 5;
  static constexpr uint8_t kRowFrontlightBrightness = 6;
  static constexpr uint8_t kRowFrontlightColor = 7;
  static constexpr uint8_t kRowHomeKeySensitivity = 8;
  static constexpr uint8_t kRowBtWakeAutotune = 9;
  static constexpr uint8_t kRowRtcCalFilter = 10;
  static constexpr uint8_t kMaxRows = 11;

  // Fills `rows` with the row ids visible on this board, in display order,
  // and returns the count. Always includes the six fixed rows; the frontlight
  // rows are appended only when the hardware supports them.
  static uint8_t activeRows(uint8_t* rows);

  void moveSelection(int8_t direction);
  void toggleSelectedSetting();
  void setFrontlightBrightnessValue(uint8_t percent);
  void setFrontlightColorValue(uint8_t percent);
  static String nextStartupPage(const String& current);
  static String nextRefreshMode(const String& current);

  uint8_t selectedRow_ = 0;
  bool dirty_ = false;
};


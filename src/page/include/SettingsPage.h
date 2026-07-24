#pragma once

#include "Page.h"

class SettingsPage final : public Page {
 public:
  explicit SettingsPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  bool handleButton(ButtonPressKind kind) override;
  void onLeave() override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  static constexpr uint8_t kEditableRows = 6;

  void toggleSelectedSetting();
  static String nextStartupPage(const String& current);
  static String nextRefreshMode(const String& current);

  uint8_t selectedRow_ = 0;
  bool dirty_ = false;
};

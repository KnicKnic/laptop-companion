#pragma once

#include "Page.h"

#include <string>

class CompanionPage final : public Page {
 public:
  explicit CompanionPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  void onEnter() override;
  bool handleButton(ButtonPressKind kind) override;
  bool handleTouch(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) override;
  void onLeave() override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  const char* triStateText(uint8_t state, const char* offText, const char* onText) const;
  bool ensureStarted();

  bool started_ = false;
  std::string actionMessage_;
};

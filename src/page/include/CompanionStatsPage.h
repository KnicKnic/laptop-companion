#pragma once

#include "Page.h"

#include <cstdint>

class CompanionStatsPage final : public Page {
 public:
  explicit CompanionStatsPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  bool handleButton(ButtonPressKind kind) override;
  void onEnter() override;
  void onLeave() override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  uint8_t scrollOffset_ = 0;
};

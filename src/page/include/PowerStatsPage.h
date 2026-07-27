#pragma once

#include "Page.h"

#include <cstdint>

class PowerStatsPage final : public Page {
 public:
  explicit PowerStatsPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  bool visible() const override;
  bool handleButton(ButtonPressKind kind) override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  uint8_t scrollOffset_ = 0;
};

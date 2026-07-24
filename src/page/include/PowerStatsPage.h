#pragma once

#include "Page.h"

class PowerStatsPage final : public Page {
 public:
  explicit PowerStatsPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  bool visible() const override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;
};

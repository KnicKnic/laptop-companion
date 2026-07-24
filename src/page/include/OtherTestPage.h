#pragma once

#include "Page.h"

class OtherTestPage final : public Page {
 public:
  explicit OtherTestPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;
};

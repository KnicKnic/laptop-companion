#pragma once

#include "Page.h"

class MainPage final : public Page {
 public:
  explicit MainPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;
};

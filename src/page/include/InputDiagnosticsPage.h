#pragma once

#include "Page.h"

class InputDiagnosticsPage final : public Page {
 public:
  explicit InputDiagnosticsPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  bool handleButton(ButtonPressKind kind) override;
  bool handleTouch(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  freeink::ui::Rect refreshRect(freeink::ui::DisplayTarget& target) const;
  bool homeDownRenderPending_ = false;
};

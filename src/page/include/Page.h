#pragma once

#include "IsrInput.h"
#include "PageId.h"
#include "RenderTransaction.h"

#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>
#include <memory>

class Page {
 public:
  explicit Page(EInkDisplay& display);
  virtual ~Page() = default;

  virtual PageId id() const = 0;
  virtual const char* name() const = 0;
  virtual bool visible() const;
  virtual void onEnter();
  virtual bool handleButton(ButtonPressKind kind);
  virtual void onLeave();
  virtual void preRender(freeink::ui::DisplayTarget& target, EInkDisplay::RefreshMode mode, bool forceDraw);
  virtual std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                                    EInkDisplay::RefreshMode mode, bool forceDraw) = 0;
  virtual void postRender(freeink::ui::DisplayTarget& target, EInkDisplay::RefreshMode mode, bool forceDraw);

 protected:
  std::unique_ptr<RenderTransaction> beginRender(EInkDisplay::RefreshMode mode);
  bool isX3Mode() const;

 private:
  EInkDisplay& display_;
};

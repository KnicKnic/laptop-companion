#include "Page.h"

Page::Page(EInkDisplay& display) : display_(display) {}

bool Page::visible() const {
  return true;
}

void Page::onEnter() {}

bool Page::handleButton(ButtonPressKind) {
  return false;
}

void Page::onLeave() {}

void Page::preRender(freeink::ui::DisplayTarget&, EInkDisplay::RefreshMode, bool) {}

std::unique_ptr<RenderTransaction> Page::beginRender(EInkDisplay::RefreshMode mode) {
  return std::make_unique<RenderTransaction>(display_, mode);
}

bool Page::isX3Mode() const {
  return display_.isX3Mode();
}

void Page::postRender(freeink::ui::DisplayTarget&, EInkDisplay::RefreshMode, bool) {}

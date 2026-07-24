#include "RenderTransaction.h"

RenderTransaction::RenderTransaction(EInkDisplay& display, EInkDisplay::RefreshMode mode)
    : display_(&display), mode_(mode), armed_(true) {
  display_->clearScreen(0xFF);
}

RenderTransaction::~RenderTransaction() {
  if (armed_ && display_ != nullptr) {
    display_->displayBuffer(mode_);
  }
}

RenderTransaction::RenderTransaction(RenderTransaction&& other) noexcept
    : display_(other.display_), mode_(other.mode_), armed_(other.armed_) {
  other.armed_ = false;
  other.display_ = nullptr;
}

void RenderTransaction::disarm() {
  armed_ = false;
}

void RenderTransaction::clearScreen(uint8_t color) {
  if (display_ != nullptr) {
    display_->clearScreen(color);
  }
}

uint8_t* RenderTransaction::getFrameBuffer() {
  return display_ == nullptr ? nullptr : display_->getFrameBuffer();
}

uint16_t RenderTransaction::getDisplayWidth() const {
  return display_ == nullptr ? 0 : display_->getDisplayWidth();
}

uint16_t RenderTransaction::getDisplayHeight() const {
  return display_ == nullptr ? 0 : display_->getDisplayHeight();
}

uint16_t RenderTransaction::getDisplayWidthBytes() const {
  return display_ == nullptr ? 0 : display_->getDisplayWidthBytes();
}

bool RenderTransaction::isX3Mode() const {
  return display_ != nullptr && display_->isX3Mode();
}

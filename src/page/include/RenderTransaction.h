#pragma once

#include <EInkDisplay.h>

class RenderTransaction {
 public:
  RenderTransaction(EInkDisplay& display, EInkDisplay::RefreshMode mode);
  ~RenderTransaction();

  RenderTransaction(const RenderTransaction&) = delete;
  RenderTransaction& operator=(const RenderTransaction&) = delete;
  RenderTransaction(RenderTransaction&& other) noexcept;
  RenderTransaction& operator=(RenderTransaction&&) = delete;

  void disarm();
  void clearScreen(uint8_t color);

  uint8_t* getFrameBuffer();
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  bool isX3Mode() const;

 private:
  EInkDisplay* display_ = nullptr;
  EInkDisplay::RefreshMode mode_ = EInkDisplay::FAST_REFRESH;
  bool armed_ = false;
};

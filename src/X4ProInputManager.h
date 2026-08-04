#pragma once

#include <Arduino.h>

// X4 Pro-only input backend. It intentionally mirrors the subset of
// InputManager consumed by IsrInput.cpp while using the GT911's GPIO10 IRQ to
// avoid continuous I2C polling during idle.
class X4ProInputManager {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
  // Compatibility-only: the app's inactive GPIO-interrupt fallback references
  // these legacy Xteink ADC pins.
  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;

  void begin();
  void update();
  bool wasPressed(uint8_t button) const;
  bool isPowerButtonPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasTouchTap(float& x, float& y) const;
  bool wasSwipe(float& xStart, float& yStart, float& xEnd, float& yEnd) const;

 private:
  struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
  };

  static void IRAM_ATTR irqTrampoline(void* self);
  void IRAM_ATTR signalIrq();
  bool takeIrq();
  bool readReg(uint16_t reg, uint8_t* data, uint8_t length);
  void clearStatus();
  void pollTouch(unsigned long now);
  void finishTouch(unsigned long now);
  bool isPressed(uint8_t button) const;

  volatile bool irqPending_ = false;
  portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t gt911Address_ = 0;
  unsigned long ignorePowerUntil_ = 0;
  uint8_t buttonState_ = 0;
  uint8_t pressedEvents_ = 0;
  bool homeDown_ = false;
  bool homeTapEvent_ = false;
  bool touchDown_ = false;
  bool touchTapEvent_ = false;
  bool swipeEvent_ = false;
  Point down_{};
  Point last_{};
  unsigned long touchDownAt_ = 0;
  float tapX_ = 0.0f;
  float tapY_ = 0.0f;
  float swipeStartX_ = 0.0f;
  float swipeStartY_ = 0.0f;
  float swipeEndX_ = 0.0f;
  float swipeEndY_ = 0.0f;

  static constexpr uint16_t kStatusReg = 0x814E;
  static constexpr uint16_t kPointReg = 0x8150;
  static constexpr uint16_t kPanelWidth = 800;
  static constexpr uint16_t kPanelHeight = 480;
  static constexpr int kTapSlopPx = 28;
  static constexpr int kSwipeMinPx = 60;
  static constexpr unsigned long kSwipeMaxMs = 700;
  static constexpr unsigned long kIgnoreBootPowerMs = 5000;
};

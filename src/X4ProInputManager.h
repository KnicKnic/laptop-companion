#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// X4 Pro-only input backend. GPIO0, GPIO7, GPIO3 and the GT911 INT line are
// interrupt-driven and wake automatic light sleep; I2C is only touched after
// the GT911 raises its interrupt.
class X4ProInputManager {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;

  void begin();
  void update();
  bool wasPressed(uint8_t button) const;
  bool isPowerButtonPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasTouchTap(float& x, float& y) const;
  bool wasSwipe(float& xStart, float& yStart, float& xEnd, float& yEnd) const;
  bool waitForEvent(TickType_t timeoutTicks);
  uint32_t touchIrqCount() const;
  uint32_t buttonIrqCount() const;

 private:
  struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
  };

  static void IRAM_ATTR touchIrqTrampoline(void* self);
  static void IRAM_ATTR buttonIrqTrampoline(void* self);
  void IRAM_ATTR signalTouchIrq();
  void IRAM_ATTR signalButtonIrq();
  bool takeTouchIrq();
  void rearmReleasedButtonInterrupts();
  void enableInterruptForPin(int8_t pin) const;
  bool readReg(uint16_t reg, uint8_t* data, uint8_t length);
  void clearStatus();
  void pollTouch(unsigned long now);
  void finishTouch(unsigned long now);
  bool isPressed(uint8_t button) const;

  volatile bool touchIrqPending_ = false;
  volatile bool touchRetryPending_ = false;
  volatile bool eventPending_ = false;
  volatile uint32_t touchIrqCount_ = 0;
  volatile uint32_t buttonIrqCount_ = 0;
  portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t eventTask_ = nullptr;
  int8_t touchIrqPin_ = -1;
  int8_t buttonPins_[3] = {-1, -1, -1};
  bool buttonRearmPending_ = false;
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
  static constexpr TickType_t kTouchRetryTicks = pdMS_TO_TICKS(15);
  static constexpr TickType_t kButtonRearmTicks = pdMS_TO_TICKS(5);
};

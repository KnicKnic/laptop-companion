#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Counters are deliberately collected below the UI layer so an unexpected
// Home-key event is preserved even while an e-ink refresh has the UI blocked.
struct X4ProInputDiagnostics {
  bool controllerDetected = false;
  bool keyConfigRead = false;
  bool homeKeyDown = false;
  uint8_t gt911Address = 0;
  uint8_t configVersion = 0;
  uint8_t keyTouchLevel = 0;
  uint8_t keyLeaveLevel = 0;
  uint8_t keySensitivity12 = 0;
  uint8_t keySensitivity34 = 0;
  uint8_t keyRestrain = 0;
  uint8_t keyRestrainTime = 0;
  uint8_t lastStatus = 0;
  uint32_t statusReads = 0;
  uint32_t statusReadFailures = 0;
  uint32_t pointReadFailures = 0;
  uint32_t homeKeyDownTransitions = 0;
  uint32_t homeKeyReleaseEvents = 0;
  uint32_t lastHomeKeyDownAtMs = 0;
  uint32_t lastHomeKeyReleaseAtMs = 0;
  uint8_t lastHomeKeyDownStatus = 0;
  uint8_t lastHomeKeyReleaseStatus = 0;
  uint8_t lastHomeKeyDownContactCount = 0;
  uint8_t lastHomeKeyReleaseContactCount = 0;
};

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
  bool wasHomeKeyPressed();
  bool wasHomeKeyTapped();
  bool wasTouchTap(float& x, float& y);
  bool wasSwipe(float& xStart, float& yStart, float& xEnd, float& yEnd);
  bool waitForEvent(TickType_t timeoutTicks);
  uint32_t touchIrqCount() const;
  uint32_t buttonIrqCount() const;
  X4ProInputDiagnostics copyDiagnostics() const;

 private:
  struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
  };

  static void IRAM_ATTR touchIrqTrampoline(void* self);
  static void IRAM_ATTR buttonIrqTrampoline(void* self);
  static void touchTaskTrampoline(void* self);
  void IRAM_ATTR signalTouchIrq();
  void IRAM_ATTR signalButtonIrq();
  void touchTask();
  void notifyEventTask();
  bool takeTouchIrq();
  void rearmTouchInterruptAfterDrain();
  void rearmReleasedButtonInterrupts();
  void enableInterruptForPin(int8_t pin) const;
  bool readReg(uint16_t reg, uint8_t* data, uint8_t length);
  bool writeReg(uint16_t reg, const uint8_t* data, uint8_t length);
  void readLoadedKeyConfig();
  void applyKeySensitivity(uint8_t sensitivity);
  void clearStatus();
  void pollTouch(unsigned long now);
  void finishTouch(unsigned long now);
  bool isPressed(uint8_t button) const;

  volatile bool touchIrqPending_ = false;
  volatile bool touchRetryPending_ = false;
  volatile bool touchInterruptMasked_ = false;
  volatile bool eventPending_ = false;
  volatile uint32_t touchIrqCount_ = 0;
  volatile uint32_t buttonIrqCount_ = 0;
  volatile uint8_t lastGt911Status_ = 0;
  volatile uint32_t statusReadCount_ = 0;
  volatile uint32_t statusReadFailureCount_ = 0;
  volatile uint32_t pointReadFailureCount_ = 0;
  volatile uint32_t homeKeyDownTransitionCount_ = 0;
  volatile uint32_t homeKeyReleaseEventCount_ = 0;
  volatile uint32_t lastHomeKeyDownAtMs_ = 0;
  volatile uint32_t lastHomeKeyReleaseAtMs_ = 0;
  volatile uint8_t lastHomeKeyDownStatus_ = 0;
  volatile uint8_t lastHomeKeyReleaseStatus_ = 0;
  volatile uint8_t lastHomeKeyDownContactCount_ = 0;
  volatile uint8_t lastHomeKeyReleaseContactCount_ = 0;
  volatile bool keyConfigRead_ = false;
  volatile uint8_t configVersion_ = 0;
  volatile uint8_t keyTouchLevel_ = 0;
  volatile uint8_t keyLeaveLevel_ = 0;
  volatile uint8_t keySensitivity12_ = 0;
  volatile uint8_t keySensitivity34_ = 0;
  volatile uint8_t keyRestrain_ = 0;
  volatile uint8_t keyRestrainTime_ = 0;
  mutable portMUX_TYPE irqMux_ = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t eventTask_ = nullptr;
  TaskHandle_t touchTask_ = nullptr;
  int8_t touchIrqPin_ = -1;
  int8_t buttonPins_[3] = {-1, -1, -1};
  bool buttonRearmPending_ = false;
  bool touchDrainActive_ = false;
  uint8_t gt911Address_ = 0;
  unsigned long ignorePowerUntil_ = 0;
  uint8_t buttonState_ = 0;
  uint8_t pressedEvents_ = 0;
  bool homeDown_ = false;
  bool homeDownEvent_ = false;
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
  static constexpr uint16_t kConfigVersionReg = 0x8047;
  static constexpr uint16_t kConfigLastReg = 0x80FE;
  static constexpr uint16_t kConfigChecksumReg = 0x80FF;
  static constexpr uint16_t kConfigFreshReg = 0x8100;
  static constexpr uint16_t kKeyConfigReg = 0x8098;
  static constexpr uint16_t kKeySensitivity12Reg = 0x809A;
  static constexpr uint16_t kKeySensitivity34Reg = 0x809B;
  static constexpr uint8_t kConfiguredKeySensitivity = 9;
  static constexpr uint8_t kConfigWriteChunkBytes = 24;
  static constexpr uint16_t kPanelWidth = 800;
  static constexpr uint16_t kPanelHeight = 480;
  static constexpr int kTapSlopPx = 28;
  static constexpr int kSwipeMinPx = 60;
  static constexpr unsigned long kSwipeMaxMs = 700;
  static constexpr unsigned long kIgnoreBootPowerMs = 5000;
  static constexpr TickType_t kTouchRetryTicks = pdMS_TO_TICKS(5);
  static constexpr TickType_t kButtonRearmTicks = pdMS_TO_TICKS(5);
  static constexpr uint16_t kTouchTaskStackWords = 4096;
  static constexpr UBaseType_t kTouchTaskPriority = 2;
};

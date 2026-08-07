#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

enum class ButtonPressKind : uint8_t {
  Gpio1,
  Gpio2,
  Power,
  Back,
  Confirm,
  Left,
  Right,
  Up,
  Down,
  Touch,
  Directory,
};

struct ButtonPress {
  ButtonPressKind kind = ButtonPressKind::Gpio1;
  float touchX = 0.0f;
  float touchY = 0.0f;
};

bool beginInput();
bool powerButtonPressed();
bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks);
bool consumeInputEvents(ButtonPress& press, TickType_t timeoutTicks = portMAX_DELAY);

// These are nonzero only for the interrupt-backed X4 Pro input backend.
uint32_t inputTouchInterruptCount();
uint32_t inputButtonInterruptCount();
const char* inputBackendName();

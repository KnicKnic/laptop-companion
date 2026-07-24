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
};

struct ButtonPress {
  ButtonPressKind kind = ButtonPressKind::Gpio1;
};

bool beginInput();
bool powerButtonPressed();
bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks);
bool consumeInputEvents(ButtonPress& press);

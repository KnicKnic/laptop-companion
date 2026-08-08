#include "IsrInput.h"

#include "AppLog.h"
#include "AppState.h"

#include <InputManager.h>
#include <freertos/task.h>

namespace {

InputManager input;

const char* buttonPressName(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Gpio1: return "GPIO1";
    case ButtonPressKind::Gpio2: return "GPIO2";
    case ButtonPressKind::Power: return "Power";
    case ButtonPressKind::Back: return "Back";
    case ButtonPressKind::Confirm: return "Confirm";
    case ButtonPressKind::Left: return "Left";
    case ButtonPressKind::Right: return "Right";
    case ButtonPressKind::Up: return "Up";
    case ButtonPressKind::Down: return "Down";
    case ButtonPressKind::Touch: return "Touch";
    case ButtonPressKind::Directory: return "Directory";
  }
  return "Unknown";
}

ButtonPressKind buttonKindFromInput(uint8_t button) {
  switch (button) {
    case InputManager::BTN_BACK: return ButtonPressKind::Back;
    case InputManager::BTN_CONFIRM: return ButtonPressKind::Confirm;
    case InputManager::BTN_LEFT: return ButtonPressKind::Left;
    case InputManager::BTN_RIGHT: return ButtonPressKind::Right;
    case InputManager::BTN_UP: return ButtonPressKind::Up;
    case InputManager::BTN_DOWN: return ButtonPressKind::Down;
    case InputManager::BTN_POWER: return ButtonPressKind::Power;
    default: return ButtonPressKind::Power;
  }
}

uint8_t inputButtonFromKind(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Back: return InputManager::BTN_BACK;
    case ButtonPressKind::Confirm: return InputManager::BTN_CONFIRM;
    case ButtonPressKind::Left: return InputManager::BTN_LEFT;
    case ButtonPressKind::Right: return InputManager::BTN_RIGHT;
    case ButtonPressKind::Up: return InputManager::BTN_UP;
    case ButtonPressKind::Down: return InputManager::BTN_DOWN;
    case ButtonPressKind::Power: return InputManager::BTN_POWER;
    default: return InputManager::BTN_POWER;
  }
}

void recordButtonPress(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Gpio1: recordGpio1Down(); break;
    case ButtonPressKind::Gpio2: recordGpio2Down(); break;
    case ButtonPressKind::Power: recordPowerInterrupt(); break;
    case ButtonPressKind::Back:
    case ButtonPressKind::Confirm:
    case ButtonPressKind::Left:
    case ButtonPressKind::Right: recordGpio1ButtonPress(inputButtonFromKind(kind)); break;
    case ButtonPressKind::Up:
    case ButtonPressKind::Down: recordGpio2ButtonPress(inputButtonFromKind(kind)); break;
    case ButtonPressKind::Touch:
    case ButtonPressKind::Directory: break;
  }
}

}  // namespace

bool beginInput() {
  input.begin();
  // E-paper refreshes can block the UI task.  The SDK task keeps sampling the
  // GT911 and latches completed taps until the page loop routes them.
  input.beginAsync(2, 15, 32);
  logPrintf("Input workflow: SDK InputManager async GPIO/GT911 queue.\n");
  return true;
}

bool powerButtonPressed() { return input.isPowerButtonPressed(); }

bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
  const TickType_t startedAt = xTaskGetTickCount();
  for (;;) {
    uint8_t button = 0;
    if (input.popPress(button)) {
      press = ButtonPress{buttonKindFromInput(button)};
      return true;
    }

    float touchX = 0.0f;
    float touchY = 0.0f;
    if (input.popTouchTap(touchX, touchY)) {
      press = ButtonPress{ButtonPressKind::Touch, touchX, touchY};
      return true;
    }

    if (input.popHomeKeyTap()) {
      press = ButtonPress{ButtonPressKind::Directory};
      return true;
    }

    float swipeXStart = 0.0f;
    float swipeYStart = 0.0f;
    float swipeXEnd = 0.0f;
    float swipeYEnd = 0.0f;
    if (input.popSwipe(swipeXStart, swipeYStart, swipeXEnd, swipeYEnd)) {
      press = ButtonPress{swipeXEnd > swipeXStart ? ButtonPressKind::Down : ButtonPressKind::Up};
      return true;
    }

    if (timeoutTicks != portMAX_DELAY && xTaskGetTickCount() - startedAt >= timeoutTicks) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool consumeInputEvents(ButtonPress& press, TickType_t timeoutTicks) {
  if (!getNextButtonPress(press, timeoutTicks)) return false;
  recordButtonPress(press.kind);
  logPrintf("Button pressed: %s\n", buttonPressName(press.kind));
  return true;
}

uint32_t inputTouchInterruptCount() { return 0; }
uint32_t inputButtonInterruptCount() { return 0; }
const char* inputBackendName() { return "SDK InputManager async polling"; }

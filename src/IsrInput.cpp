#include "IsrInput.h"

#include "AppLog.h"
#include "AppState.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
#include "X4ProInputManager.h"
using ActiveInputManager = X4ProInputManager;
#else
#include <InputManager.h>
using ActiveInputManager = InputManager;
#endif

namespace {

ActiveInputManager input;
portMUX_TYPE diagnosticMux = portMUX_INITIALIZER_UNLOCKED;
InputDiagnosticEvent recentEvents[kInputDiagnosticEventCapacity];
uint8_t recentEventHead = 0;
uint8_t recentEventCount = 0;
uint32_t deliveredEventCount = 0;
uint32_t directoryEventCount = 0;
uint32_t lastDirectoryEventAtMs = 0;

void recordDiagnosticEvent(ButtonPressKind kind) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&diagnosticMux);
  recentEvents[recentEventHead] = InputDiagnosticEvent{kind, now};
  recentEventHead = static_cast<uint8_t>((recentEventHead + 1) % kInputDiagnosticEventCapacity);
  if (recentEventCount < kInputDiagnosticEventCapacity) ++recentEventCount;
  ++deliveredEventCount;
  if (kind == ButtonPressKind::Directory) {
    ++directoryEventCount;
    lastDirectoryEventAtMs = now;
  }
  portEXIT_CRITICAL(&diagnosticMux);
}

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
    case ButtonPressKind::HomeKeyDown: return "Home key down";
  }
  return "Unknown";
}

ButtonPressKind buttonKindFromInput(uint8_t button) {
  switch (button) {
    case ActiveInputManager::BTN_BACK: return ButtonPressKind::Back;
    case ActiveInputManager::BTN_CONFIRM: return ButtonPressKind::Confirm;
    case ActiveInputManager::BTN_LEFT: return ButtonPressKind::Left;
    case ActiveInputManager::BTN_RIGHT: return ButtonPressKind::Right;
    case ActiveInputManager::BTN_UP: return ButtonPressKind::Up;
    case ActiveInputManager::BTN_DOWN: return ButtonPressKind::Down;
    case ActiveInputManager::BTN_POWER: return ButtonPressKind::Power;
    default: return ButtonPressKind::Power;
  }
}

uint8_t inputButtonFromKind(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Back: return ActiveInputManager::BTN_BACK;
    case ButtonPressKind::Confirm: return ActiveInputManager::BTN_CONFIRM;
    case ButtonPressKind::Left: return ActiveInputManager::BTN_LEFT;
    case ButtonPressKind::Right: return ActiveInputManager::BTN_RIGHT;
    case ButtonPressKind::Up: return ActiveInputManager::BTN_UP;
    case ButtonPressKind::Down: return ActiveInputManager::BTN_DOWN;
    case ButtonPressKind::Power: return ActiveInputManager::BTN_POWER;
    default: return ActiveInputManager::BTN_POWER;
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
    case ButtonPressKind::Directory:
    case ButtonPressKind::HomeKeyDown: break;
  }
}

}  // namespace

bool beginInput() {
  input.begin();
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  logPrintf("Input workflow: X4 Pro interrupt GPIO/GT911 backend.\n");
#else
  // E-paper refreshes can block the UI task.  The SDK task keeps sampling the
  // GT911 and latches completed taps until the page loop routes them.
  input.beginAsync(2, 15, 32);
  logPrintf("Input workflow: SDK InputManager async GPIO/GT911 queue.\n");
#endif
  return true;
}

bool powerButtonPressed() { return input.isPowerButtonPressed(); }

bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  const TickType_t startedAt = xTaskGetTickCount();
  for (;;) {
    input.update();
    for (uint8_t button : {ActiveInputManager::BTN_BACK, ActiveInputManager::BTN_CONFIRM,
                           ActiveInputManager::BTN_LEFT, ActiveInputManager::BTN_RIGHT,
                           ActiveInputManager::BTN_UP, ActiveInputManager::BTN_DOWN,
                           ActiveInputManager::BTN_POWER}) {
      if (input.wasPressed(button)) {
        press = ButtonPress{buttonKindFromInput(button)};
        return true;
      }
    }

    float touchX = 0.0f;
    float touchY = 0.0f;
    if (input.wasTouchTap(touchX, touchY)) {
      press = ButtonPress{ButtonPressKind::Touch, touchX, touchY};
      return true;
    }
    if (input.wasHomeKeyPressed()) {
      press = ButtonPress{ButtonPressKind::HomeKeyDown};
      return true;
    }
    if (input.wasHomeKeyTapped()) {
      press = ButtonPress{ButtonPressKind::Directory};
      return true;
    }

    float swipeXStart = 0.0f;
    float swipeYStart = 0.0f;
    float swipeXEnd = 0.0f;
    float swipeYEnd = 0.0f;
    if (input.wasSwipe(swipeXStart, swipeYStart, swipeXEnd, swipeYEnd)) {
      press = ButtonPress{swipeXEnd > swipeXStart ? ButtonPressKind::Down : ButtonPressKind::Up};
      return true;
    }

    if (timeoutTicks != portMAX_DELAY) {
      const TickType_t elapsed = xTaskGetTickCount() - startedAt;
      if (elapsed >= timeoutTicks) return false;
      if (!input.waitForEvent(timeoutTicks - elapsed)) return false;
    } else {
      input.waitForEvent(portMAX_DELAY);
    }
  }
#else
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
#endif
}

bool consumeInputEvents(ButtonPress& press, TickType_t timeoutTicks) {
  if (!getNextButtonPress(press, timeoutTicks)) return false;
  recordButtonPress(press.kind);
  recordDiagnosticEvent(press.kind);
  logPrintf("Button pressed: %s\n", buttonPressName(press.kind));
  return true;
}

uint32_t inputTouchInterruptCount() {
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  return input.touchIrqCount();
#else
  return 0;
#endif
}
uint32_t inputButtonInterruptCount() {
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  return input.buttonIrqCount();
#else
  return 0;
#endif
}
const char* inputBackendName() {
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  return "X4 Pro GPIO/GT911 interrupts";
#else
  return "SDK InputManager async polling";
#endif
}

InputDiagnosticsSnapshot copyInputDiagnostics() {
  InputDiagnosticsSnapshot snapshot;
  portENTER_CRITICAL(&diagnosticMux);
  snapshot.deliveredEvents = deliveredEventCount;
  snapshot.directoryEvents = directoryEventCount;
  snapshot.lastDirectoryEventAtMs = lastDirectoryEventAtMs;
  snapshot.recentEventCount = recentEventCount;
  for (uint8_t i = 0; i < recentEventCount; ++i) {
    const uint8_t index = static_cast<uint8_t>(
        (recentEventHead + kInputDiagnosticEventCapacity - 1 - i) % kInputDiagnosticEventCapacity);
    snapshot.recentEvents[i] = recentEvents[index];
  }
  portEXIT_CRITICAL(&diagnosticMux);

  snapshot.touchInterrupts = inputTouchInterruptCount();
  snapshot.buttonInterrupts = inputButtonInterruptCount();
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  const X4ProInputDiagnostics x4 = input.copyDiagnostics();
  snapshot.gt911Detected = x4.controllerDetected;
  snapshot.gt911KeyConfigRead = x4.keyConfigRead;
  snapshot.homeKeyDown = x4.homeKeyDown;
  snapshot.gt911Address = x4.gt911Address;
  snapshot.gt911ConfigVersion = x4.configVersion;
  snapshot.gt911KeyTouchLevel = x4.keyTouchLevel;
  snapshot.gt911KeyLeaveLevel = x4.keyLeaveLevel;
  snapshot.gt911KeySensitivity12 = x4.keySensitivity12;
  snapshot.gt911KeySensitivity34 = x4.keySensitivity34;
  snapshot.gt911KeyRestrain = x4.keyRestrain;
  snapshot.gt911KeyRestrainTime = x4.keyRestrainTime;
  snapshot.lastGt911Status = x4.lastStatus;
  snapshot.gt911StatusReads = x4.statusReads;
  snapshot.gt911StatusReadFailures = x4.statusReadFailures;
  snapshot.gt911PointReadFailures = x4.pointReadFailures;
  snapshot.homeKeyDownTransitions = x4.homeKeyDownTransitions;
  snapshot.homeKeyReleaseEvents = x4.homeKeyReleaseEvents;
  snapshot.lastHomeKeyDownAtMs = x4.lastHomeKeyDownAtMs;
  snapshot.lastHomeKeyReleaseAtMs = x4.lastHomeKeyReleaseAtMs;
  snapshot.lastHomeKeyDownStatus = x4.lastHomeKeyDownStatus;
  snapshot.lastHomeKeyReleaseStatus = x4.lastHomeKeyReleaseStatus;
  snapshot.lastHomeKeyDownContactCount = x4.lastHomeKeyDownContactCount;
  snapshot.lastHomeKeyReleaseContactCount = x4.lastHomeKeyReleaseContactCount;
  snapshot.lastHomeTransitionWasDown = x4.lastHomeKeyDownAtMs > x4.lastHomeKeyReleaseAtMs;
#endif
  return snapshot;
}

const char* inputPressKindName(ButtonPressKind kind) {
  return buttonPressName(kind);
}

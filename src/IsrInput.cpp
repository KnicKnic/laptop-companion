#include "IsrInput.h"

#include "AppLog.h"
#include "AppState.h"
#include "X4ProInputManager.h"

#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

constexpr uint8_t kButtonQueueLen = 16;

const char* buttonPressName(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Gpio1:
      return "GPIO1";
    case ButtonPressKind::Gpio2:
      return "GPIO2";
    case ButtonPressKind::Power:
      return "Power";
    case ButtonPressKind::Back:
      return "Back";
    case ButtonPressKind::Confirm:
      return "Confirm";
    case ButtonPressKind::Left:
      return "Left";
    case ButtonPressKind::Right:
      return "Right";
    case ButtonPressKind::Up:
      return "Up";
    case ButtonPressKind::Down:
      return "Down";
    case ButtonPressKind::Touch:
      return "Touch";
    case ButtonPressKind::Directory:
      return "Directory";
  }
  return "Unknown";
}

ButtonPressKind buttonKindFromInput(uint8_t button) {
  switch (button) {
    case X4ProInputManager::BTN_BACK:
      return ButtonPressKind::Back;
    case X4ProInputManager::BTN_CONFIRM:
      return ButtonPressKind::Confirm;
    case X4ProInputManager::BTN_LEFT:
      return ButtonPressKind::Left;
    case X4ProInputManager::BTN_RIGHT:
      return ButtonPressKind::Right;
    case X4ProInputManager::BTN_UP:
      return ButtonPressKind::Up;
    case X4ProInputManager::BTN_DOWN:
      return ButtonPressKind::Down;
    case X4ProInputManager::BTN_POWER:
      return ButtonPressKind::Power;
  }
  return ButtonPressKind::Power;
}

uint8_t inputButtonFromKind(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Back:
      return X4ProInputManager::BTN_BACK;
    case ButtonPressKind::Confirm:
      return X4ProInputManager::BTN_CONFIRM;
    case ButtonPressKind::Left:
      return X4ProInputManager::BTN_LEFT;
    case ButtonPressKind::Right:
      return X4ProInputManager::BTN_RIGHT;
    case ButtonPressKind::Up:
      return X4ProInputManager::BTN_UP;
    case ButtonPressKind::Down:
      return X4ProInputManager::BTN_DOWN;
    case ButtonPressKind::Power:
      return X4ProInputManager::BTN_POWER;
    case ButtonPressKind::Gpio1:
    case ButtonPressKind::Gpio2:
    case ButtonPressKind::Touch:
    case ButtonPressKind::Directory:
      return X4ProInputManager::BTN_POWER;
  }
  return X4ProInputManager::BTN_POWER;
}

void recordButtonPress(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Gpio1:
      recordGpio1Down();
      break;
    case ButtonPressKind::Gpio2:
      recordGpio2Down();
      break;
    case ButtonPressKind::Power:
      recordPowerInterrupt();
      break;
    case ButtonPressKind::Back:
    case ButtonPressKind::Confirm:
    case ButtonPressKind::Left:
    case ButtonPressKind::Right:
      recordGpio1ButtonPress(inputButtonFromKind(kind));
      break;
    case ButtonPressKind::Up:
    case ButtonPressKind::Down:
      recordGpio2ButtonPress(inputButtonFromKind(kind));
      break;
    case ButtonPressKind::Touch:
    case ButtonPressKind::Directory:
      break;
  }
}

// X4 Pro has three direct active-low buttons and a GT911 which asserts INT low
// until its status frame is acknowledged.  The manager configures every one
// with ONLOW_WE: it is an interrupt source while awake and a GPIO wake source
// during automatic light sleep.  There is no idle polling task.
class X4ProInterruptPressSource {
 public:
  bool begin() {
    queue_ = xQueueCreate(kButtonQueueLen, sizeof(ButtonPress));
    if (!queue_) return false;

    input_.begin();
    logPrintf("Input workflow: X4 Pro GPIO/GT911 interrupts with light-sleep wake.\n");
    return true;
  }

  bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
    if (!queue_) return false;
    const TickType_t startedAt = xTaskGetTickCount();
    TickType_t remainingTicks = timeoutTicks;

    for (;;) {
      input_.update();
      enqueueInputEvents();
      if (xQueueReceive(queue_, &press, 0) == pdTRUE) return true;

      // waitForEvent may use a shorter internal timeout to retry a failed
      // GT911 transaction or to re-arm a released button. Those maintenance
      // deadlines must not be mistaken for the caller's input deadline.
      input_.waitForEvent(remainingTicks);
      if (!updateRemainingTicks(startedAt, timeoutTicks, remainingTicks)) return false;
    }
  }

  bool powerButtonPressed() const {
    return input_.isPowerButtonPressed();
  }

 private:
  static bool updateRemainingTicks(TickType_t startedAt, TickType_t timeoutTicks, TickType_t& remainingTicks) {
    if (timeoutTicks == portMAX_DELAY) {
      remainingTicks = portMAX_DELAY;
      return true;
    }

    const TickType_t elapsed = xTaskGetTickCount() - startedAt;
    if (elapsed >= timeoutTicks) return false;
    remainingTicks = timeoutTicks - elapsed;
    return true;
  }

  void enqueueInputEvents() {
    static const uint8_t buttons[] = {X4ProInputManager::BTN_BACK,  X4ProInputManager::BTN_CONFIRM,
                                      X4ProInputManager::BTN_LEFT,  X4ProInputManager::BTN_RIGHT,
                                      X4ProInputManager::BTN_UP,    X4ProInputManager::BTN_DOWN,
                                      X4ProInputManager::BTN_POWER};
    for (uint8_t button : buttons) {
      if (input_.wasPressed(button)) {
        const ButtonPress event{buttonKindFromInput(button)};
        xQueueSend(queue_, &event, 0);
      }
    }

    if (input_.wasHomeKeyTapped()) {
      const ButtonPress event{ButtonPressKind::Directory};
      xQueueSend(queue_, &event, 0);
    }

    float touchX = 0.0f;
    float touchY = 0.0f;
    if (input_.wasTouchTap(touchX, touchY)) {
      const ButtonPress event{ButtonPressKind::Touch, touchX, touchY};
      xQueueSend(queue_, &event, 0);
    }

    float swipeXStart = 0.0f;
    float swipeYStart = 0.0f;
    float swipeXEnd = 0.0f;
    float swipeYEnd = 0.0f;
    if (input_.wasSwipe(swipeXStart, swipeYStart, swipeXEnd, swipeYEnd)) {
      const ButtonPressKind kind = swipeXEnd > swipeXStart ? ButtonPressKind::Down : ButtonPressKind::Up;
      const ButtonPress event{kind};
      xQueueSend(queue_, &event, 0);
    }
  }

  X4ProInputManager input_;
  QueueHandle_t queue_ = nullptr;
};

X4ProInterruptPressSource x4ProInput;

}  // namespace

bool beginInput() {
  return x4ProInput.begin();
}

bool powerButtonPressed() {
  return x4ProInput.powerButtonPressed();
}

bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
  return x4ProInput.getNextButtonPress(press, timeoutTicks);
}

bool consumeInputEvents(ButtonPress& press, TickType_t timeoutTicks) {
  if (!getNextButtonPress(press, timeoutTicks)) {
    return false;
  }

  recordButtonPress(press.kind);
  logPrintf("Button pressed: %s\n", buttonPressName(press.kind));
  return true;
}

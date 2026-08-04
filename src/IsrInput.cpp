#include "IsrInput.h"

#include "AppLog.h"
#include "AppState.h"
#include "X4ProInputManager.h"

#include <BoardConfig.h>
#include <driver/gpio.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

using InputManager = X4ProInputManager;

enum class InputWorkflow : uint8_t {
  AdcButtons,
  GpioInterrupts,
};

constexpr InputWorkflow kInputWorkflow = InputWorkflow::AdcButtons;
constexpr uint32_t kReleaseSettleMs = 5;
constexpr uint32_t kAdcPollMs = 15;
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

ButtonPressKind buttonKindFromInputManager(uint8_t button) {
  switch (button) {
    case InputManager::BTN_BACK:
      return ButtonPressKind::Back;
    case InputManager::BTN_CONFIRM:
      return ButtonPressKind::Confirm;
    case InputManager::BTN_LEFT:
      return ButtonPressKind::Left;
    case InputManager::BTN_RIGHT:
      return ButtonPressKind::Right;
    case InputManager::BTN_UP:
      return ButtonPressKind::Up;
    case InputManager::BTN_DOWN:
      return ButtonPressKind::Down;
    case InputManager::BTN_POWER:
      return ButtonPressKind::Power;
  }
  return ButtonPressKind::Power;
}

uint8_t inputManagerButtonFromKind(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Back:
      return InputManager::BTN_BACK;
    case ButtonPressKind::Confirm:
      return InputManager::BTN_CONFIRM;
    case ButtonPressKind::Left:
      return InputManager::BTN_LEFT;
    case ButtonPressKind::Right:
      return InputManager::BTN_RIGHT;
    case ButtonPressKind::Up:
      return InputManager::BTN_UP;
    case ButtonPressKind::Down:
      return InputManager::BTN_DOWN;
    case ButtonPressKind::Power:
      return InputManager::BTN_POWER;
    case ButtonPressKind::Gpio1:
    case ButtonPressKind::Gpio2:
    case ButtonPressKind::Touch:
    case ButtonPressKind::Directory:
      return InputManager::BTN_POWER;
  }
  return InputManager::BTN_POWER;
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
      recordGpio1ButtonPress(inputManagerButtonFromKind(kind));
      break;
    case ButtonPressKind::Up:
    case ButtonPressKind::Down:
      recordGpio2ButtonPress(inputManagerButtonFromKind(kind));
      break;
    case ButtonPressKind::Touch:
    case ButtonPressKind::Directory:
      break;
  }
}

class AdcButtonPressSource {
 public:
  bool begin() {
    queue_ = xQueueCreate(kButtonQueueLen, sizeof(ButtonPress));
    if (!queue_) return false;

    input_.begin();
    xTaskCreate(taskTrampoline, "adc_buttons", 4096, this, 2, &task_);
    if (!task_) return false;

    logPrintf("Input workflow: InputManager polling task (digital buttons, touch, and Home key).\n");
    return true;
  }

  bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
    if (!queue_) return false;
    return xQueueReceive(queue_, &press, timeoutTicks) == pdTRUE;
  }

  bool powerButtonPressed() const {
    return input_.isPowerButtonPressed();
  }

 private:
  static void taskTrampoline(void* self) {
    static_cast<AdcButtonPressSource*>(self)->pollLoop();
  }

  void pollLoop() {
    static const uint8_t buttons[] = {InputManager::BTN_BACK,  InputManager::BTN_CONFIRM, InputManager::BTN_LEFT,
                                      InputManager::BTN_RIGHT, InputManager::BTN_UP,      InputManager::BTN_DOWN,
                                      InputManager::BTN_POWER};
    for (;;) {
      input_.update();
      for (const uint8_t button : buttons) {
        if (input_.wasPressed(button)) {
          ButtonPress press{buttonKindFromInputManager(button)};
          xQueueSend(queue_, &press, 0);
        }
      }
      if (input_.wasHomeKeyTapped()) {
        const ButtonPress press{ButtonPressKind::Directory};
        xQueueSend(queue_, &press, 0);
      }
      float touchX = 0.0f;
      float touchY = 0.0f;
      if (input_.wasTouchTap(touchX, touchY)) {
        const ButtonPress press{ButtonPressKind::Touch, touchX, touchY};
        xQueueSend(queue_, &press, 0);
      }
      float swipeXStart = 0.0f;
      float swipeYStart = 0.0f;
      float swipeXEnd = 0.0f;
      float swipeYEnd = 0.0f;
      if (input_.wasSwipe(swipeXStart, swipeYStart, swipeXEnd, swipeYEnd)) {
        const ButtonPressKind kind = swipeXEnd > swipeXStart ? ButtonPressKind::Down : ButtonPressKind::Up;
        const ButtonPress press{kind};
        xQueueSend(queue_, &press, 0);
      }
      vTaskDelay(pdMS_TO_TICKS(kAdcPollMs));
    }
  }

  InputManager input_;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
};

class GpioButtonPressSource {
 public:
  bool begin() {
    instance_ = this;
    queue_ = xQueueCreate(kButtonQueueLen, sizeof(ButtonPress));
    if (!queue_) return false;

    powerInput_.pin = BoardConfig::ACTIVE.input.power;
    powerInterruptPin_ = powerInput_.pin;

    configureInput(gpio1Input_, onGpio1Low);
    configureInput(gpio2Input_, onGpio2Low);
    configureInput(powerInput_, onPowerLow);
    logPrintf("Input workflow: GPIO ONLOW_WE interrupts; ADC reads disabled.\n");
    return true;
  }

  bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
    rearmReleasedInputs();

    const TickType_t startedAt = xTaskGetTickCount();
    TickType_t remainingTicks = timeoutTicks;
    for (;;) {
      ButtonPress signaledPress;
      if (xQueueReceive(queue_, &signaledPress, remainingTicks) != pdTRUE) {
        return false;
      }

      GpioDownInput* input = inputForKind(signaledPress.kind);
      if (!input) continue;

      if (!digitalInputPressed(*input)) {
        enableInterruptForPin(input->pin);
        if (!updateRemainingTicks(startedAt, timeoutTicks, remainingTicks)) return false;
        continue;
      }

      input->waitingForRelease = true;
      press = signaledPress;
      logPrintf("%s ONLOW_WE ISR confirmed low\n", buttonPressName(press.kind));
      return true;
    }
  }

  bool powerButtonPressed() const {
    return digitalInputPressed(powerInput_);
  }

 private:
  struct GpioDownInput {
    int8_t pin = -1;
    ButtonPressKind kind = ButtonPressKind::Gpio1;
    bool waitingForRelease = false;
  };

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

  static void IRAM_ATTR disableInterruptForPin(int8_t pin) {
    if (pin >= 0) {
      gpio_intr_disable(static_cast<gpio_num_t>(pin));
    }
  }

  static void enableInterruptForPin(int8_t pin) {
    if (pin >= 0) {
      gpio_intr_enable(static_cast<gpio_num_t>(pin));
    }
  }

  static bool digitalInputPressed(const GpioDownInput& input) {
    return input.pin >= 0 && digitalRead(input.pin) == LOW;
  }

  static void configureInput(const GpioDownInput& input, void (*isr)()) {
    if (input.pin < 0) return;

    pinMode(input.pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(input.pin), isr, ONLOW_WE);
  }

  static void IRAM_ATTR onGpio1Low() {
    if (instance_) instance_->signalFromIsr(ButtonPressKind::Gpio1, InputManager::BUTTON_ADC_PIN_1);
  }

  static void IRAM_ATTR onGpio2Low() {
    if (instance_) instance_->signalFromIsr(ButtonPressKind::Gpio2, InputManager::BUTTON_ADC_PIN_2);
  }

  static void IRAM_ATTR onPowerLow() {
    if (instance_) instance_->signalFromIsr(ButtonPressKind::Power, instance_->powerInterruptPin_);
  }

  void IRAM_ATTR signalFromIsr(ButtonPressKind kind, int8_t pin) {
    disableInterruptForPin(pin);
    if (!queue_) return;

    ButtonPress press{kind};
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(queue_, &press, &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }

  GpioDownInput* inputForKind(ButtonPressKind kind) {
    switch (kind) {
      case ButtonPressKind::Gpio1:
        return &gpio1Input_;
      case ButtonPressKind::Gpio2:
        return &gpio2Input_;
      case ButtonPressKind::Power:
        return &powerInput_;
      case ButtonPressKind::Back:
      case ButtonPressKind::Confirm:
      case ButtonPressKind::Left:
      case ButtonPressKind::Right:
      case ButtonPressKind::Up:
      case ButtonPressKind::Down:
      case ButtonPressKind::Touch:
      case ButtonPressKind::Directory:
        return nullptr;
    }
    return nullptr;
  }

  void rearmReleasedInputs() {
    rearmReleasedInput(gpio1Input_);
    rearmReleasedInput(gpio2Input_);
    rearmReleasedInput(powerInput_);
  }

  void rearmReleasedInput(GpioDownInput& input) {
    if (!input.waitingForRelease) return;

    for (;;) {
      while (digitalInputPressed(input)) {
        vTaskDelay(pdMS_TO_TICKS(kReleaseSettleMs));
      }

      vTaskDelay(pdMS_TO_TICKS(kReleaseSettleMs));
      if (!digitalInputPressed(input)) {
        input.waitingForRelease = false;
        enableInterruptForPin(input.pin);
        return;
      }
    }
  }

  static GpioButtonPressSource* instance_;

  QueueHandle_t queue_ = nullptr;
  int8_t powerInterruptPin_ = -1;
  GpioDownInput gpio1Input_{InputManager::BUTTON_ADC_PIN_1, ButtonPressKind::Gpio1, false};
  GpioDownInput gpio2Input_{InputManager::BUTTON_ADC_PIN_2, ButtonPressKind::Gpio2, false};
  GpioDownInput powerInput_{-1, ButtonPressKind::Power, false};
};

GpioButtonPressSource* GpioButtonPressSource::instance_ = nullptr;

AdcButtonPressSource adcButtons;
GpioButtonPressSource gpioButtons;

}  // namespace

bool beginInput() {
  if (kInputWorkflow == InputWorkflow::GpioInterrupts) {
    return gpioButtons.begin();
  }
  return adcButtons.begin();
}

bool powerButtonPressed() {
  if (kInputWorkflow == InputWorkflow::GpioInterrupts) {
    return gpioButtons.powerButtonPressed();
  }
  return adcButtons.powerButtonPressed();
}

bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks) {
  if (kInputWorkflow == InputWorkflow::GpioInterrupts) {
    return gpioButtons.getNextButtonPress(press, timeoutTicks);
  }
  return adcButtons.getNextButtonPress(press, timeoutTicks);
}

bool consumeInputEvents(ButtonPress& press, TickType_t timeoutTicks) {
  if (!getNextButtonPress(press, timeoutTicks)) {
    return false;
  }

  recordButtonPress(press.kind);
  logPrintf("Button pressed: %s\n", buttonPressName(press.kind));
  return true;
}

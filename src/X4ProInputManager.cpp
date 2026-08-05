#include "X4ProInputManager.h"

#include "AppLog.h"
#include "PowerManagement.h"

#include <BoardConfig.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace {
int absolute(int value) { return value < 0 ? -value : value; }
constexpr uint32_t kTouchAwakeWindowMs = 2000;
TimedPowerManagementLock touchAwakeLock(ESP_PM_NO_LIGHT_SLEEP, "x4-touch");
}  // namespace

void X4ProInputManager::begin() {
  ignorePowerUntil_ = millis() + kIgnoreBootPowerMs;
  const auto& t = BoardConfig::ACTIVE.touch;
  if (t.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(t.powerEnable));
    pinMode(t.powerEnable, OUTPUT);
    digitalWrite(t.powerEnable, t.powerEnableActiveHigh ? HIGH : LOW);
    delay(50);
  }
  Wire.begin(t.sda, t.scl, 400000);
  Wire.setTimeOut(10);

  auto resetWithIntLevel = [&](uint8_t level) {
    pinMode(t.irq, OUTPUT);
    pinMode(t.reset, OUTPUT);
    digitalWrite(t.reset, LOW);
    digitalWrite(t.irq, level);
    delay(10);
    digitalWrite(t.reset, HIGH);
    delay(10);
    digitalWrite(t.irq, level);
    delay(50);
    pinMode(t.irq, INPUT);
    delay(50);
  };
  auto probe = [&]() {
    for (uint8_t address : {t.i2cAddress, t.i2cAddressAlt}) {
      if (address == 0) continue;
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) {
        gt911Address_ = address;
        return true;
      }
    }
    return false;
  };
  resetWithIntLevel(LOW);
  if (!probe()) {
    resetWithIntLevel(HIGH);
    probe();
  }

  uint8_t wakePins = 0;
  auto attachWakeInterrupt = [&](int8_t pin, void (*isr)(void*)) {
    if (pin < 0) return;
    // ONLOW_WE arms the Arduino ISR and its pad wake bit. Repeat the driver
    // call here so an unsupported wake pad is reported rather than silently
    // leaving a wake path unarmed.
    attachInterruptArg(digitalPinToInterrupt(pin), isr, this, ONLOW_WE);
    const esp_err_t err = gpio_wakeup_enable(static_cast<gpio_num_t>(pin), GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
      logPrintf("X4 input: GPIO%d cannot wake light sleep: %s\n", pin, esp_err_to_name(err));
      return;
    }
    wakePins++;
  };
  if (gt911Address_ != 0) {
    touchIrqPin_ = t.irq;
    touchIrqPending_ = true;
    eventPending_ = true;
    attachWakeInterrupt(touchIrqPin_, touchIrqTrampoline);
  }

  const auto& input = BoardConfig::ACTIVE.input;
  const uint8_t buttons[] = {BTN_UP, BTN_DOWN, BTN_POWER};
  for (uint8_t index = 0; index < sizeof(buttons); index++) {
    const uint8_t button = buttons[index];
    const int8_t pin = button == BTN_UP ? input.up : (button == BTN_DOWN ? input.down : input.power);
    if (pin >= 0) {
      pinMode(pin, INPUT_PULLUP);
      buttonPins_[index] = pin;
      attachWakeInterrupt(pin, buttonIrqTrampoline);
    }
    if (isPressed(button)) buttonState_ |= static_cast<uint8_t>(1U << button);
  }
  const esp_err_t wakeResult = wakePins == 0 ? ESP_ERR_INVALID_STATE : esp_sleep_enable_gpio_wakeup();
  if (wakeResult != ESP_OK) {
    logPrintf("X4 input: GPIO light-sleep wake unavailable: %s\n", esp_err_to_name(wakeResult));
  } else {
    logPrintf("X4 input: %u GPIO interrupt wake sources armed.\n", wakePins);
  }
}

void X4ProInputManager::update() {
  touchAwakeLock.releaseIfExpired();
  pressedEvents_ = 0;
  homeTapEvent_ = false;
  touchTapEvent_ = false;
  swipeEvent_ = false;
  uint8_t nextState = 0;
  for (uint8_t button : {BTN_UP, BTN_DOWN, BTN_POWER}) {
    if (isPressed(button)) nextState |= static_cast<uint8_t>(1U << button);
  }
  pressedEvents_ = static_cast<uint8_t>(nextState & ~buttonState_);
  if (static_cast<long>(millis() - ignorePowerUntil_) < 0) pressedEvents_ &= static_cast<uint8_t>(~(1U << BTN_POWER));
  buttonState_ = nextState;
  rearmReleasedButtonInterrupts();
  pollTouch(millis());
}

bool X4ProInputManager::wasPressed(uint8_t button) const { return (pressedEvents_ & (1U << button)) != 0; }
bool X4ProInputManager::isPowerButtonPressed() const { return (buttonState_ & (1U << BTN_POWER)) != 0; }
bool X4ProInputManager::wasHomeKeyTapped() const { return homeTapEvent_; }
bool X4ProInputManager::wasTouchTap(float& x, float& y) const {
  if (!touchTapEvent_) return false;
  x = tapX_;
  y = tapY_;
  return true;
}
bool X4ProInputManager::wasSwipe(float& xStart, float& yStart, float& xEnd, float& yEnd) const {
  if (!swipeEvent_) return false;
  xStart = swipeStartX_;
  yStart = swipeStartY_;
  xEnd = swipeEndX_;
  yEnd = swipeEndY_;
  return true;
}

bool X4ProInputManager::waitForEvent(TickType_t timeoutTicks) {
  const uint32_t awakeMs = touchAwakeLock.millisecondsLeft();
  const TickType_t awakeTicks = awakeMs == 0 ? 0 : pdMS_TO_TICKS(awakeMs);
  portENTER_CRITICAL(&irqMux_);
  eventTask_ = xTaskGetCurrentTaskHandle();
  const bool pending = eventPending_;
  TickType_t effectiveTimeout = buttonRearmPending_ ? kButtonRearmTicks : (touchRetryPending_ ? kTouchRetryTicks : timeoutTicks);
  if (awakeTicks != 0 && (effectiveTimeout == portMAX_DELAY || awakeTicks < effectiveTimeout)) effectiveTimeout = awakeTicks;
  eventPending_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return pending || ulTaskNotifyTake(pdTRUE, effectiveTimeout) != 0;
}

uint32_t X4ProInputManager::touchIrqCount() const { return touchIrqCount_; }
uint32_t X4ProInputManager::buttonIrqCount() const { return buttonIrqCount_; }
void X4ProInputManager::touchIrqTrampoline(void* self) { static_cast<X4ProInputManager*>(self)->signalTouchIrq(); }
void X4ProInputManager::buttonIrqTrampoline(void* self) { static_cast<X4ProInputManager*>(self)->signalButtonIrq(); }

void X4ProInputManager::signalTouchIrq() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL_ISR(&irqMux_);
  if (touchIrqPin_ >= 0) gpio_intr_disable(static_cast<gpio_num_t>(touchIrqPin_));
  touchIrqPending_ = true;
  eventPending_ = true;
  touchIrqCount_ = touchIrqCount_ + 1U;
  task = eventTask_;
  portEXIT_CRITICAL_ISR(&irqMux_);
  BaseType_t woken = pdFALSE;
  if (task) vTaskNotifyGiveFromISR(task, &woken);
  if (woken == pdTRUE) portYIELD_FROM_ISR();
}

void X4ProInputManager::signalButtonIrq() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL_ISR(&irqMux_);
  for (int8_t pin : buttonPins_) if (pin >= 0) gpio_intr_disable(static_cast<gpio_num_t>(pin));
  eventPending_ = true;
  buttonRearmPending_ = true;
  buttonIrqCount_ = buttonIrqCount_ + 1U;
  task = eventTask_;
  portEXIT_CRITICAL_ISR(&irqMux_);
  BaseType_t woken = pdFALSE;
  if (task) vTaskNotifyGiveFromISR(task, &woken);
  if (woken == pdTRUE) portYIELD_FROM_ISR();
}

bool X4ProInputManager::takeTouchIrq() {
  portENTER_CRITICAL(&irqMux_);
  const bool pending = touchIrqPending_;
  touchIrqPending_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return pending;
}
void X4ProInputManager::enableInterruptForPin(int8_t pin) const {
  if (pin >= 0) gpio_intr_enable(static_cast<gpio_num_t>(pin));
}
void X4ProInputManager::rearmReleasedButtonInterrupts() {
  if (!buttonRearmPending_) return;
  for (uint8_t button : {BTN_UP, BTN_DOWN, BTN_POWER}) if (isPressed(button)) return;
  for (int8_t pin : buttonPins_) enableInterruptForPin(pin);
  buttonRearmPending_ = false;
}

bool X4ProInputManager::readReg(uint16_t reg, uint8_t* data, uint8_t length) {
  Wire.beginTransmission(gt911Address_);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(gt911Address_, length, static_cast<uint8_t>(true)) != length) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < length; i++) data[i] = Wire.read();
  return true;
}
void X4ProInputManager::clearStatus() {
  Wire.beginTransmission(gt911Address_);
  Wire.write(static_cast<uint8_t>(kStatusReg >> 8));
  Wire.write(static_cast<uint8_t>(kStatusReg));
  Wire.write(static_cast<uint8_t>(0));
  Wire.endTransmission();
}

void X4ProInputManager::pollTouch(unsigned long now) {
  if (gt911Address_ == 0 || !takeTouchIrq()) return;
  touchAwakeLock.ensureArmed(kTouchAwakeWindowMs);
  uint8_t status = 0;
  if (!readReg(kStatusReg, &status, 1)) {
    portENTER_CRITICAL(&irqMux_);
    touchIrqPending_ = true;
    touchRetryPending_ = true;
    portEXIT_CRITICAL(&irqMux_);
    return;
  }
  touchRetryPending_ = false;
  if ((status & 0x80) == 0) {
    enableInterruptForPin(touchIrqPin_);
    return;
  }
  const bool homeDown = (status & 0x10) != 0;
  if (!homeDown && homeDown_) homeTapEvent_ = true;
  homeDown_ = homeDown;
  const uint8_t count = status & 0x0F;
  if (count == 0) {
    finishTouch(now);
    clearStatus();
    enableInterruptForPin(touchIrqPin_);
    return;
  }
  uint8_t point[8] = {};
  if (readReg(kPointReg, point, sizeof(point))) {
    const uint8_t offset = BoardConfig::ACTIVE.touch.gt911CoordsAtByte0 ? 0 : 1;
    const uint16_t rawX = static_cast<uint16_t>(point[offset]) | (static_cast<uint16_t>(point[offset + 1]) << 8);
    const uint16_t rawY = static_cast<uint16_t>(point[offset + 2]) | (static_cast<uint16_t>(point[offset + 3]) << 8);
    const auto& t = BoardConfig::ACTIVE.touch;
    uint16_t x = t.swapXY ? rawY : rawX;
    uint16_t y = t.swapXY ? rawX : rawY;
    x = x > t.rawMaxX ? t.rawMaxX : x;
    y = y > t.rawMaxY ? t.rawMaxY : y;
    if (t.flipX) x = static_cast<uint16_t>(t.rawMaxX - x);
    if (t.flipY) y = static_cast<uint16_t>(t.rawMaxY - y);
    const Point mapped{x, y};
    if (!touchDown_) {
      touchDown_ = true;
      down_ = mapped;
      touchDownAt_ = now;
    }
    last_ = mapped;
  }
  clearStatus();
  enableInterruptForPin(touchIrqPin_);
}

void X4ProInputManager::finishTouch(unsigned long now) {
  if (!touchDown_) return;
  const int dx = static_cast<int>(last_.x) - static_cast<int>(down_.x);
  const int dy = static_cast<int>(last_.y) - static_cast<int>(down_.y);
  const float startX = static_cast<float>(down_.x) / kPanelWidth;
  const float startY = static_cast<float>(down_.y) / kPanelHeight;
  const float endX = static_cast<float>(last_.x) / kPanelWidth;
  const float endY = static_cast<float>(last_.y) / kPanelHeight;
  if (absolute(dx) <= kTapSlopPx && absolute(dy) <= kTapSlopPx) {
    tapX_ = startX;
    tapY_ = startY;
    touchTapEvent_ = true;
  } else if (now - touchDownAt_ <= kSwipeMaxMs && (absolute(dx) >= kSwipeMinPx || absolute(dy) >= kSwipeMinPx)) {
    swipeStartX_ = startX;
    swipeStartY_ = startY;
    swipeEndX_ = endX;
    swipeEndY_ = endY;
    swipeEvent_ = true;
  }
  touchDown_ = false;
}

bool X4ProInputManager::isPressed(uint8_t button) const {
  const auto& input = BoardConfig::ACTIVE.input;
  int8_t pin = -1;
  if (button == BTN_UP) pin = input.up;
  if (button == BTN_DOWN) pin = input.down;
  if (button == BTN_POWER) pin = input.power;
  if (pin < 0) return false;
  const int active = input.powerActiveHigh && button == BTN_POWER ? HIGH : LOW;
  return digitalRead(pin) == active;
}

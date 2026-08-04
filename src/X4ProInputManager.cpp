#include "X4ProInputManager.h"

#include <BoardConfig.h>
#include <Wire.h>
#include <driver/gpio.h>

namespace {
int absolute(int value) { return value < 0 ? -value : value; }
}

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

  auto resetWithIntLevel = [&](const uint8_t level) {
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
    for (const uint8_t address : {t.i2cAddress, t.i2cAddressAlt}) {
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

  if (gt911Address_ != 0) {
    pinMode(t.irq, INPUT);
    irqPending_ = true;  // consume a status frame that arrived during boot
    attachInterruptArg(digitalPinToInterrupt(t.irq), irqTrampoline, this, FALLING);
  }

  for (const uint8_t button : {BTN_UP, BTN_DOWN, BTN_POWER}) {
    const auto& input = BoardConfig::ACTIVE.input;
    const int8_t pin = button == BTN_UP ? input.up : (button == BTN_DOWN ? input.down : input.power);
    if (pin >= 0) pinMode(pin, INPUT_PULLUP);
    if (isPressed(button)) buttonState_ |= static_cast<uint8_t>(1U << button);
  }
}

void X4ProInputManager::update() {
  pressedEvents_ = 0;
  homeTapEvent_ = false;
  touchTapEvent_ = false;
  swipeEvent_ = false;

  uint8_t nextState = 0;
  for (const uint8_t button : {BTN_UP, BTN_DOWN, BTN_POWER}) {
    if (isPressed(button)) nextState |= static_cast<uint8_t>(1U << button);
  }
  pressedEvents_ = static_cast<uint8_t>(nextState & ~buttonState_);
  if (static_cast<long>(millis() - ignorePowerUntil_) < 0) {
    pressedEvents_ &= static_cast<uint8_t>(~(1U << BTN_POWER));
  }
  buttonState_ = nextState;
  pollTouch(millis());
}

bool X4ProInputManager::wasPressed(const uint8_t button) const {
  return (pressedEvents_ & static_cast<uint8_t>(1U << button)) != 0;
}

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

void X4ProInputManager::irqTrampoline(void* self) {
  static_cast<X4ProInputManager*>(self)->signalIrq();
}

void X4ProInputManager::signalIrq() {
  portENTER_CRITICAL_ISR(&irqMux_);
  irqPending_ = true;
  portEXIT_CRITICAL_ISR(&irqMux_);
}

bool X4ProInputManager::takeIrq() {
  portENTER_CRITICAL(&irqMux_);
  const bool pending = irqPending_;
  irqPending_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return pending;
}

bool X4ProInputManager::readReg(const uint16_t reg, uint8_t* data, const uint8_t length) {
  Wire.beginTransmission(gt911Address_);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(gt911Address_, length, static_cast<uint8_t>(true)) != length) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < length; ++i) data[i] = Wire.read();
  return true;
}

void X4ProInputManager::clearStatus() {
  Wire.beginTransmission(gt911Address_);
  Wire.write(static_cast<uint8_t>(kStatusReg >> 8));
  Wire.write(static_cast<uint8_t>(kStatusReg));
  Wire.write(static_cast<uint8_t>(0));
  Wire.endTransmission();
}

void X4ProInputManager::pollTouch(const unsigned long now) {
  if (gt911Address_ == 0 || !takeIrq()) return;

  uint8_t status = 0;
  if (!readReg(kStatusReg, &status, 1)) {
    portENTER_CRITICAL(&irqMux_);
    irqPending_ = true;
    portEXIT_CRITICAL(&irqMux_);
    return;
  }
  if ((status & 0x80) == 0) return;

  const bool homeDown = (status & 0x10) != 0;
  if (!homeDown && homeDown_) homeTapEvent_ = true;
  homeDown_ = homeDown;

  const uint8_t count = status & 0x0F;
  if (count == 0) {
    finishTouch(now);
    clearStatus();
    return;
  }

  uint8_t point[8] = {};
  if (readReg(kPointReg, point, sizeof(point))) {
    const uint8_t offset = BoardConfig::ACTIVE.touch.gt911CoordsAtByte0 ? 0 : 1;
    const uint16_t rawX = static_cast<uint16_t>(point[offset]) | (static_cast<uint16_t>(point[offset + 1]) << 8);
    const uint16_t rawY = static_cast<uint16_t>(point[offset + 2]) | (static_cast<uint16_t>(point[offset + 3]) << 8);
    const auto& t = BoardConfig::ACTIVE.touch;
    const uint16_t panelX = t.swapXY ? rawY : rawX;
    const uint16_t panelY = t.swapXY ? rawX : rawY;
    Point mapped{};
    mapped.x = panelX > t.rawMaxX ? t.rawMaxX : panelX;
    mapped.y = panelY > t.rawMaxY ? t.rawMaxY : panelY;
    if (t.flipX) mapped.x = static_cast<uint16_t>(t.rawMaxX - mapped.x);
    if (t.flipY) mapped.y = static_cast<uint16_t>(t.rawMaxY - mapped.y);
    if (!touchDown_) {
      touchDown_ = true;
      down_ = mapped;
      touchDownAt_ = now;
    }
    last_ = mapped;
  }
  clearStatus();
}

void X4ProInputManager::finishTouch(const unsigned long now) {
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
  } else if (now - touchDownAt_ <= kSwipeMaxMs &&
             (absolute(dx) >= kSwipeMinPx || absolute(dy) >= kSwipeMinPx)) {
    swipeStartX_ = startX;
    swipeStartY_ = startY;
    swipeEndX_ = endX;
    swipeEndY_ = endY;
    swipeEvent_ = true;
  }
  touchDown_ = false;
}

bool X4ProInputManager::isPressed(const uint8_t button) const {
  const auto& input = BoardConfig::ACTIVE.input;
  int8_t pin = -1;
  if (button == BTN_UP) pin = input.up;
  if (button == BTN_DOWN) pin = input.down;
  if (button == BTN_POWER) pin = input.power;
  if (pin < 0) return false;
  const int active = input.powerActiveHigh && button == BTN_POWER ? HIGH : LOW;
  return digitalRead(pin) == active;
}

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
  if (gt911Address_ != 0) {
    readLoadedKeyConfig();
    applyKeySensitivity(kConfiguredKeySensitivity);
    readLoadedKeyConfig();
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
    attachWakeInterrupt(touchIrqPin_, touchIrqTrampoline);
    if (xTaskCreate(touchTaskTrampoline, "x4-touch", kTouchTaskStackWords, this, kTouchTaskPriority, &touchTask_) !=
        pdPASS) {
      touchTask_ = nullptr;
      eventPending_ = true;
      logPrintf("X4 input: touch drain task failed; falling back to the UI task.\n");
    } else {
      // Drain any GT911 status already asserted during boot from the dedicated
      // task, never from the GPIO ISR.
      xTaskNotifyGive(touchTask_);
    }
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
  uint8_t nextState = 0;
  for (uint8_t button : {BTN_UP, BTN_DOWN, BTN_POWER}) {
    if (isPressed(button)) nextState |= static_cast<uint8_t>(1U << button);
  }
  pressedEvents_ = static_cast<uint8_t>(nextState & ~buttonState_);
  if (static_cast<long>(millis() - ignorePowerUntil_) < 0) pressedEvents_ &= static_cast<uint8_t>(~(1U << BTN_POWER));
  buttonState_ = nextState;
  rearmReleasedButtonInterrupts();
  if (touchTask_ == nullptr) pollTouch(millis());
}

bool X4ProInputManager::wasPressed(uint8_t button) const { return (pressedEvents_ & (1U << button)) != 0; }
bool X4ProInputManager::isPowerButtonPressed() const { return (buttonState_ & (1U << BTN_POWER)) != 0; }
bool X4ProInputManager::wasHomeKeyPressed() {
  portENTER_CRITICAL(&irqMux_);
  const bool pressed = homeDownEvent_;
  homeDownEvent_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return pressed;
}
bool X4ProInputManager::wasHomeKeyTapped() {
  portENTER_CRITICAL(&irqMux_);
  const bool tapped = homeTapEvent_;
  homeTapEvent_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return tapped;
}
bool X4ProInputManager::wasTouchTap(float& x, float& y) {
  portENTER_CRITICAL(&irqMux_);
  if (!touchTapEvent_) {
    portEXIT_CRITICAL(&irqMux_);
    return false;
  }
  x = tapX_;
  y = tapY_;
  touchTapEvent_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return true;
}
bool X4ProInputManager::wasSwipe(float& xStart, float& yStart, float& xEnd, float& yEnd) {
  portENTER_CRITICAL(&irqMux_);
  if (!swipeEvent_) {
    portEXIT_CRITICAL(&irqMux_);
    return false;
  }
  xStart = swipeStartX_;
  yStart = swipeStartY_;
  xEnd = swipeEndX_;
  yEnd = swipeEndY_;
  swipeEvent_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return true;
}

bool X4ProInputManager::waitForEvent(TickType_t timeoutTicks) {
  const uint32_t awakeMs = touchAwakeLock.millisecondsLeft();
  const TickType_t awakeTicks = awakeMs == 0 ? 0 : pdMS_TO_TICKS(awakeMs);
  portENTER_CRITICAL(&irqMux_);
  eventTask_ = xTaskGetCurrentTaskHandle();
  const bool pending = eventPending_;
  TickType_t effectiveTimeout = buttonRearmPending_ ? kButtonRearmTicks : timeoutTicks;
  if (awakeTicks != 0 && (effectiveTimeout == portMAX_DELAY || awakeTicks < effectiveTimeout)) effectiveTimeout = awakeTicks;
  eventPending_ = false;
  portEXIT_CRITICAL(&irqMux_);
  return pending || ulTaskNotifyTake(pdTRUE, effectiveTimeout) != 0;
}

uint32_t X4ProInputManager::touchIrqCount() const { return touchIrqCount_; }
uint32_t X4ProInputManager::buttonIrqCount() const { return buttonIrqCount_; }
X4ProInputDiagnostics X4ProInputManager::copyDiagnostics() const {
  X4ProInputDiagnostics diagnostics;
  portENTER_CRITICAL(&irqMux_);
  diagnostics.controllerDetected = gt911Address_ != 0;
  diagnostics.keyConfigRead = keyConfigRead_;
  diagnostics.homeKeyDown = homeDown_;
  diagnostics.gt911Address = gt911Address_;
  diagnostics.configVersion = configVersion_;
  diagnostics.keyTouchLevel = keyTouchLevel_;
  diagnostics.keyLeaveLevel = keyLeaveLevel_;
  diagnostics.keySensitivity12 = keySensitivity12_;
  diagnostics.keySensitivity34 = keySensitivity34_;
  diagnostics.keyRestrain = keyRestrain_;
  diagnostics.keyRestrainTime = keyRestrainTime_;
  diagnostics.lastStatus = lastGt911Status_;
  diagnostics.statusReads = statusReadCount_;
  diagnostics.statusReadFailures = statusReadFailureCount_;
  diagnostics.pointReadFailures = pointReadFailureCount_;
  diagnostics.homeKeyDownTransitions = homeKeyDownTransitionCount_;
  diagnostics.homeKeyReleaseEvents = homeKeyReleaseEventCount_;
  diagnostics.lastHomeKeyDownAtMs = lastHomeKeyDownAtMs_;
  diagnostics.lastHomeKeyReleaseAtMs = lastHomeKeyReleaseAtMs_;
  diagnostics.lastHomeKeyDownStatus = lastHomeKeyDownStatus_;
  diagnostics.lastHomeKeyReleaseStatus = lastHomeKeyReleaseStatus_;
  diagnostics.lastHomeKeyDownContactCount = lastHomeKeyDownContactCount_;
  diagnostics.lastHomeKeyReleaseContactCount = lastHomeKeyReleaseContactCount_;
  portEXIT_CRITICAL(&irqMux_);
  return diagnostics;
}
void X4ProInputManager::touchIrqTrampoline(void* self) { static_cast<X4ProInputManager*>(self)->signalTouchIrq(); }
void X4ProInputManager::buttonIrqTrampoline(void* self) { static_cast<X4ProInputManager*>(self)->signalButtonIrq(); }
void X4ProInputManager::touchTaskTrampoline(void* self) { static_cast<X4ProInputManager*>(self)->touchTask(); }

void X4ProInputManager::signalTouchIrq() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL_ISR(&irqMux_);
  // The GT911 INT pin is level-low.  Mask it before notifying the task so a
  // single asserted status line cannot repeatedly enter this ISR.
  if (touchInterruptMasked_) {
    portEXIT_CRITICAL_ISR(&irqMux_);
    return;
  }
  touchInterruptMasked_ = true;
  if (touchIrqPin_ >= 0) gpio_intr_disable(static_cast<gpio_num_t>(touchIrqPin_));
  touchIrqPending_ = true;
  touchIrqCount_ = touchIrqCount_ + 1U;
  task = touchTask_ != nullptr ? touchTask_ : eventTask_;
  if (touchTask_ == nullptr) eventPending_ = true;
  portEXIT_CRITICAL_ISR(&irqMux_);
  BaseType_t woken = pdFALSE;
  if (task) vTaskNotifyGiveFromISR(task, &woken);
  if (woken == pdTRUE) portYIELD_FROM_ISR();
}

void X4ProInputManager::touchTask() {
  for (;;) {
    const TickType_t timeout = touchRetryPending_ ? kTouchRetryTicks : portMAX_DELAY;
    ulTaskNotifyTake(pdTRUE, timeout);
    pollTouch(millis());
  }
}

void X4ProInputManager::notifyEventTask() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL(&irqMux_);
  task = eventTask_;
  portEXIT_CRITICAL(&irqMux_);
  if (task) xTaskNotifyGive(task);
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

void X4ProInputManager::rearmTouchInterruptAfterDrain() {
  if (touchIrqPin_ < 0) return;

  // Do not re-enable a level-low interrupt while INT is still asserted.  Keep
  // draining the GT911 status register from task context instead; otherwise
  // enabling the line immediately retriggers the ISR in a tight loop.
  if (digitalRead(touchIrqPin_) == LOW) {
    portENTER_CRITICAL(&irqMux_);
    touchIrqPending_ = true;
    touchRetryPending_ = true;
    portEXIT_CRITICAL(&irqMux_);
    return;
  }

  portENTER_CRITICAL(&irqMux_);
  touchInterruptMasked_ = false;
  touchRetryPending_ = false;
  portEXIT_CRITICAL(&irqMux_);
  gpio_intr_enable(static_cast<gpio_num_t>(touchIrqPin_));
  touchDrainActive_ = false;
  touchAwakeLock.release();
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

bool X4ProInputManager::writeReg(uint16_t reg, const uint8_t* data, uint8_t length) {
  Wire.beginTransmission(gt911Address_);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg));
  if (Wire.write(data, length) != length) return false;
  return Wire.endTransmission() == 0;
}

void X4ProInputManager::readLoadedKeyConfig() {
  uint8_t configVersion = 0;
  uint8_t keyConfig[6] = {};
  if (!readReg(kConfigVersionReg, &configVersion, 1) || !readReg(kKeyConfigReg, keyConfig, sizeof(keyConfig))) {
    logPrintf("GT911: unable to read loaded touch-key configuration.\n");
    return;
  }

  portENTER_CRITICAL(&irqMux_);
  keyConfigRead_ = true;
  configVersion_ = configVersion;
  keyTouchLevel_ = keyConfig[0];
  keyLeaveLevel_ = keyConfig[1];
  keySensitivity12_ = keyConfig[2];
  keySensitivity34_ = keyConfig[3];
  keyRestrain_ = keyConfig[4];
  keyRestrainTime_ = keyConfig[5];
  portEXIT_CRITICAL(&irqMux_);
  logPrintf("GT911 loaded key config: version=0x%02X touch=%u leave=%u sens=0x%02X/0x%02X restrain=0x%02X/0x%02X\n",
            configVersion, keyConfig[0], keyConfig[1], keyConfig[2], keyConfig[3], keyConfig[4], keyConfig[5]);
}

void X4ProInputManager::applyKeySensitivity(uint8_t sensitivity) {
  constexpr uint16_t kConfigLength = kConfigLastReg - kConfigVersionReg + 1;
  uint8_t config[kConfigLength] = {};
  for (uint16_t offset = 0; offset < kConfigLength; offset += kConfigWriteChunkBytes) {
    const uint8_t chunk = static_cast<uint8_t>(
        (kConfigLength - offset) < kConfigWriteChunkBytes ? (kConfigLength - offset) : kConfigWriteChunkBytes);
    if (!readReg(static_cast<uint16_t>(kConfigVersionReg + offset), config + offset, chunk)) {
      logPrintf("GT911: unable to read configuration at 0x%04X before applying key sensitivity.\n",
                static_cast<unsigned>(kConfigVersionReg + offset));
      return;
    }
  }

  const uint8_t packedSensitivity = static_cast<uint8_t>((sensitivity << 4) | sensitivity);
  const uint16_t sensitivity12Offset = kKeySensitivity12Reg - kConfigVersionReg;
  const uint16_t sensitivity34Offset = kKeySensitivity34Reg - kConfigVersionReg;
  if (config[sensitivity12Offset] == packedSensitivity && config[sensitivity34Offset] == packedSensitivity) {
    logPrintf("GT911: key sensitivity already %u (0x%02X/0x%02X).\n", sensitivity, packedSensitivity,
              packedSensitivity);
    return;
  }

  // GT911 accepts a host-updated configuration when its version is zero. The
  // controller self-loads its panel configuration on each boot, so this
  // startup override is intentionally reapplied rather than persisted.
  config[0] = 0;
  config[sensitivity12Offset] = packedSensitivity;
  config[sensitivity34Offset] = packedSensitivity;

  uint8_t checksum = 0;
  for (uint16_t i = 0; i < kConfigLength; ++i) checksum = static_cast<uint8_t>(checksum + config[i]);
  checksum = static_cast<uint8_t>(~checksum + 1U);

  for (uint16_t offset = 0; offset < kConfigLength; offset += kConfigWriteChunkBytes) {
    const uint8_t chunk = static_cast<uint8_t>(
        (kConfigLength - offset) < kConfigWriteChunkBytes ? (kConfigLength - offset) : kConfigWriteChunkBytes);
    if (!writeReg(static_cast<uint16_t>(kConfigVersionReg + offset), config + offset, chunk)) {
      logPrintf("GT911: failed writing configuration at 0x%04X; sensitivity unchanged.\n",
                static_cast<unsigned>(kConfigVersionReg + offset));
      return;
    }
  }
  if (!writeReg(kConfigChecksumReg, &checksum, 1)) {
    logPrintf("GT911: failed writing configuration checksum; sensitivity unchanged.\n");
    return;
  }
  const uint8_t fresh = 1;
  if (!writeReg(kConfigFreshReg, &fresh, 1)) {
    logPrintf("GT911: failed applying configuration; sensitivity unchanged.\n");
    return;
  }
  delay(10);
  logPrintf("GT911: applied touch-key sensitivity %u to all four key slots.\n", sensitivity);
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
  if (!touchDrainActive_) {
    touchAwakeLock.ensureArmed(kTouchAwakeWindowMs);
    touchDrainActive_ = true;
  }
  uint8_t status = 0;
  if (!readReg(kStatusReg, &status, 1)) {
    portENTER_CRITICAL(&irqMux_);
    ++statusReadFailureCount_;
    touchIrqPending_ = true;
    touchRetryPending_ = true;
    portEXIT_CRITICAL(&irqMux_);
    return;
  }
  portENTER_CRITICAL(&irqMux_);
  ++statusReadCount_;
  lastGt911Status_ = status;
  portEXIT_CRITICAL(&irqMux_);
  if ((status & 0x80) == 0) {
    clearStatus();
    rearmTouchInterruptAfterDrain();
    return;
  }
  const bool homeDown = (status & 0x10) != 0;
  if (homeDown && !homeDown_) {
    portENTER_CRITICAL(&irqMux_);
    homeDownEvent_ = true;
    ++homeKeyDownTransitionCount_;
    lastHomeKeyDownAtMs_ = now;
    lastHomeKeyDownStatus_ = status;
    lastHomeKeyDownContactCount_ = status & 0x0F;
    portEXIT_CRITICAL(&irqMux_);
    logPrintf("GT911 Home key DOWN: status=0x%02X contacts=%u uptime=%lu ms\n", status, status & 0x0F,
              static_cast<unsigned long>(now));
    notifyEventTask();
  }
  if (!homeDown && homeDown_) {
    portENTER_CRITICAL(&irqMux_);
    homeTapEvent_ = true;
    ++homeKeyReleaseEventCount_;
    lastHomeKeyReleaseAtMs_ = now;
    lastHomeKeyReleaseStatus_ = status;
    lastHomeKeyReleaseContactCount_ = status & 0x0F;
    portEXIT_CRITICAL(&irqMux_);
    logPrintf("GT911 Home key UP: status=0x%02X contacts=%u uptime=%lu ms\n", status, status & 0x0F,
              static_cast<unsigned long>(now));
    notifyEventTask();
  }
  homeDown_ = homeDown;
  const uint8_t count = status & 0x0F;
  if (count == 0) {
    finishTouch(now);
    clearStatus();
    rearmTouchInterruptAfterDrain();
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
  } else {
    portENTER_CRITICAL(&irqMux_);
    ++pointReadFailureCount_;
    portEXIT_CRITICAL(&irqMux_);
  }
  clearStatus();
  rearmTouchInterruptAfterDrain();
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
    portENTER_CRITICAL(&irqMux_);
    tapX_ = startX;
    tapY_ = startY;
    touchTapEvent_ = true;
    portEXIT_CRITICAL(&irqMux_);
    notifyEventTask();
  } else if (now - touchDownAt_ <= kSwipeMaxMs && (absolute(dx) >= kSwipeMinPx || absolute(dy) >= kSwipeMinPx)) {
    portENTER_CRITICAL(&irqMux_);
    swipeStartX_ = startX;
    swipeStartY_ = startY;
    swipeEndX_ = endX;
    swipeEndY_ = endY;
    swipeEvent_ = true;
    portEXIT_CRITICAL(&irqMux_);
    notifyEventTask();
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

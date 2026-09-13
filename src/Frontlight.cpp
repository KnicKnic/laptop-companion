#include "Frontlight.h"

#include <FrontlightManager.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

SemaphoreHandle_t frontlightMutex = nullptr;
FrontlightManager frontlight;

void lockFrontlight() {
  xSemaphoreTake(frontlightMutex, portMAX_DELAY);
}

void unlockFrontlight() {
  xSemaphoreGive(frontlightMutex);
}

}  // namespace

bool beginFrontlight() {
  frontlightMutex = xSemaphoreCreateMutex();
  if (frontlightMutex == nullptr) return false;
  frontlight.begin();
  return true;
}

bool frontlightPresent() {
  // Reads BoardConfig::ACTIVE, which is fixed at compile/boot time; no lock needed.
  return frontlight.present();
}

bool frontlightHasColorTemperature() {
  return frontlight.hasColorTemperature();
}

void setFrontlightBrightness(uint8_t percent) {
  lockFrontlight();
  frontlight.setBrightness(percent);
  unlockFrontlight();
}

uint8_t frontlightBrightness() {
  lockFrontlight();
  const uint8_t value = frontlight.brightness();
  unlockFrontlight();
  return value;
}

void setFrontlightColorTemperature(uint8_t warmPercent) {
  lockFrontlight();
  frontlight.setColorTemperature(warmPercent);
  unlockFrontlight();
}

uint8_t frontlightColorTemperature() {
  lockFrontlight();
  const uint8_t value = frontlight.colorTemperature();
  unlockFrontlight();
  return value;
}

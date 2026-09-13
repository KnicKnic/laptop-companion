#pragma once

// Thin, mutex-protected wrapper around FreeInk SDK's FrontlightManager so the
// single instance can be driven from the input/loop task (button/touch
// handling) while pages read its state back on the display task during
// render(). Inert (present() == false) on boards without a frontlight.

#include <cstdint>

bool beginFrontlight();

bool frontlightPresent();
bool frontlightHasColorTemperature();

// 0-100 percent. 0 turns the light off.
void setFrontlightBrightness(uint8_t percent);
uint8_t frontlightBrightness();

// 0 = fully cool, 100 = fully warm, 50 = neutral. No-op on single-channel
// (or absent) frontlights; frontlightHasColorTemperature() reports support.
void setFrontlightColorTemperature(uint8_t warmPercent);
uint8_t frontlightColorTemperature();

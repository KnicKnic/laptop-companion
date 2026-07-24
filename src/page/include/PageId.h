#pragma once

#include <Arduino.h>

enum class PageId : uint8_t {
  Main,
  Companion,
  CompanionStats,
  Settings,
  OtherTest,
  PowerStats,
  CompanionSettingsWarning,
};

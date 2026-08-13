#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

enum class ButtonPressKind : uint8_t {
  Gpio1,
  Gpio2,
  Power,
  Back,
  Confirm,
  Left,
  Right,
  Up,
  Down,
  Touch,
  Directory,
  HomeKeyDown,
};

struct ButtonPress {
  ButtonPressKind kind = ButtonPressKind::Gpio1;
  float touchX = 0.0f;
  float touchY = 0.0f;
};

// A compact rolling record of delivered input events.  Timestamps are device
// uptime in milliseconds, so the report remains useful without real-time
// clock configuration.
constexpr uint8_t kInputDiagnosticEventCapacity = 8;
struct InputDiagnosticEvent {
  ButtonPressKind kind = ButtonPressKind::Gpio1;
  uint32_t uptimeMs = 0;
};

struct InputDiagnosticsSnapshot {
  uint32_t deliveredEvents = 0;
  uint32_t directoryEvents = 0;
  uint32_t lastDirectoryEventAtMs = 0;
  uint32_t touchInterrupts = 0;
  uint32_t buttonInterrupts = 0;
  bool gt911Detected = false;
  bool gt911KeyConfigRead = false;
  bool homeKeyDown = false;
  uint8_t gt911Address = 0;
  uint8_t gt911ConfigVersion = 0;
  uint8_t gt911KeyTouchLevel = 0;
  uint8_t gt911KeyLeaveLevel = 0;
  uint8_t gt911KeySensitivity12 = 0;
  uint8_t gt911KeySensitivity34 = 0;
  uint8_t gt911KeyRestrain = 0;
  uint8_t gt911KeyRestrainTime = 0;
  uint8_t lastGt911Status = 0;
  uint32_t gt911StatusReads = 0;
  uint32_t gt911StatusReadFailures = 0;
  uint32_t gt911PointReadFailures = 0;
  uint32_t homeKeyDownTransitions = 0;
  uint32_t homeKeyReleaseEvents = 0;
  uint32_t lastHomeKeyDownAtMs = 0;
  uint32_t lastHomeKeyReleaseAtMs = 0;
  uint8_t lastHomeKeyDownStatus = 0;
  uint8_t lastHomeKeyReleaseStatus = 0;
  uint8_t lastHomeKeyDownContactCount = 0;
  uint8_t lastHomeKeyReleaseContactCount = 0;
  bool lastHomeTransitionWasDown = false;
  uint8_t recentEventCount = 0;
  InputDiagnosticEvent recentEvents[kInputDiagnosticEventCapacity]{};
};

bool beginInput();
bool powerButtonPressed();
bool getNextButtonPress(ButtonPress& press, TickType_t timeoutTicks);
bool consumeInputEvents(ButtonPress& press, TickType_t timeoutTicks = portMAX_DELAY);

// These are nonzero only for the interrupt-backed X4 Pro input backend.
uint32_t inputTouchInterruptCount();
uint32_t inputButtonInterruptCount();
const char* inputBackendName();
InputDiagnosticsSnapshot copyInputDiagnostics();
const char* inputPressKindName(ButtonPressKind kind);

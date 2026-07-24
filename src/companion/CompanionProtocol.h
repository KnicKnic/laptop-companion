#pragma once

#include <cstdint>

namespace CompanionProtocol {

constexpr const char* SERVICE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000001";
constexpr const char* HOST_TEAMS_STATE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000002";
constexpr const char* HOST_MICROPHONE_STATE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000003";
constexpr const char* HOST_CAMERA_STATE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000004";
constexpr const char* HOST_STATUS_MESSAGE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000005";
constexpr const char* BUTTON_EVENT_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000006";
constexpr const char* DEVICE_INFO_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000007";
constexpr const char* HOST_MEETING_STATE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000008";
constexpr const char* HOST_HAND_STATE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a000009";
constexpr const char* HOST_MEETING_NAME_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a00000a";
constexpr const char* CONNECTION_PARTICIPATION_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a00000b";

constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint16_t STATE_COUNTER_MASK = 0x7FFF;

enum class ButtonId : uint8_t {
  ToggleMute = 1,
  ToggleHand = 2,
  ToggleCamera = 3,
};

enum class ButtonAction : uint8_t {
  Released = 1,
};

enum class TriState : uint8_t {
  Unknown = 0,
  Off = 1,
  On = 2,
};

constexpr uint16_t encodeState(bool on, uint16_t counter) {
  return static_cast<uint16_t>(((counter & STATE_COUNTER_MASK) << 1) | (on ? 1U : 0U));
}

}  // namespace CompanionProtocol

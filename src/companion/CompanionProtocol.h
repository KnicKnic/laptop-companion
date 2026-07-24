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

constexpr uint8_t PROTOCOL_VERSION = 1;

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

}  // namespace CompanionProtocol

#pragma once

#include <cstddef>
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
constexpr const char* HOST_DESKTOP_STATE_UUID = "7d2d5f00-778d-4df6-a6d5-7c4e7a00000c";

constexpr uint8_t PROTOCOL_VERSION = 4;
constexpr uint16_t STATE_COUNTER_MASK = 0x3FFF;

// Virtual desktop state payload: [version][count][activeIndex][ackSeqLo][ackSeqHi] then, per
// desktop, [flags][nameLen][name bytes]. Names are UTF-8 and already truncated by the host.
// The ack sequence echoes the switch request the host last finished handling (0 when none), so
// the device can settle a pending switch whether or not it landed on the requested desktop.
constexpr size_t DESKTOP_MAX_COUNT = 8;
constexpr size_t DESKTOP_NAME_MAX_LEN = 16;
constexpr size_t DESKTOP_STATE_HEADER_LEN = 5;
constexpr size_t DESKTOP_STATE_MAX_LEN =
    DESKTOP_STATE_HEADER_LEN + DESKTOP_MAX_COUNT * (2 + DESKTOP_NAME_MAX_LEN);
constexpr uint8_t DESKTOP_INDEX_UNKNOWN = 0xFF;
constexpr uint8_t DESKTOP_FLAG_REMOTE = 0x01;
// A remote desktop that is registered with Task view but has no live session. Tapping it asks
// the host to connect rather than to switch.
constexpr uint8_t DESKTOP_FLAG_DISCONNECTED = 0x02;
constexpr uint16_t DESKTOP_ACK_NONE = 0;
// Safety net for a host that never answers a switch request.
constexpr uint32_t DESKTOP_SWITCH_TIMEOUT_MS = 15000;
// A toggle whose new state is never echoed back still has to settle, so the pressed
// styling clears itself this long after the press.
constexpr uint32_t STATE_PENDING_TIMEOUT_MS = 2000;
// Transmitting a reaction is near instant, so the pressed styling is held briefly to stay
// visible on a display that takes a moment to refresh.
constexpr uint32_t REACTION_MIN_PRESSED_MS = 1000;

constexpr size_t BUTTON_EVENT_PAYLOAD_LEN = 10;

enum class ButtonId : uint8_t {
  ToggleMute = 1,
  ToggleHand = 2,
  ToggleCamera = 3,
  SwitchDesktop = 4,
  // Meeting reactions are fire-and-forget: they carry no state, so the device never
  // waits for an acknowledging state write for them.
  ReactLike = 5,
  ReactHeart = 6,
  ReactApplause = 7,
};

constexpr bool isReactionButton(uint8_t buttonId) {
  return buttonId == static_cast<uint8_t>(ButtonId::ReactLike) ||
         buttonId == static_cast<uint8_t>(ButtonId::ReactHeart) ||
         buttonId == static_cast<uint8_t>(ButtonId::ReactApplause);
}

enum class ButtonAction : uint8_t {
  Released = 1,
};

enum class TriState : uint8_t {
  Unknown = 0,
  Off = 1,
  On = 2,
};

constexpr uint16_t encodeState(bool on, uint16_t counter, bool locked = false) {
  return static_cast<uint16_t>(((counter & STATE_COUNTER_MASK) << 2) | (locked ? 0x0002U : 0U) | (on ? 0x0001U : 0U));
}

}  // namespace CompanionProtocol

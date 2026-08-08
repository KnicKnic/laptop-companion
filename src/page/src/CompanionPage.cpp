#include "CompanionPage.h"

#include "DisplayWorker.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "Settings.h"
#include "companion/CompanionBleService.h"
#include "companion/CompanionProtocol.h"
#include "icons/CompanionIcons.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIIcon.h>

namespace {
void requestCompanionRender() {
  if (!displayWorkerReady()) return;

  requestRender(RenderKind::ActivePage, refreshModeFromSettings(copySettings()));
}

void drawIcon(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& rect, const freeink::Icon& icon,
              freeink::ui::Color color = freeink::ui::Color::Black) {
  target.bitmap(rect, freeink::ui::bitmapFromIcon(icon), freeink::ui::BitmapMode::Contain,
                freeink::ui::Paint::solid(color));
}

bool triStateIs(uint8_t state, CompanionProtocol::TriState expected) {
  return state == static_cast<uint8_t>(expected);
}

const freeink::Icon& micStatusIcon(uint8_t state) {
  return triStateIs(state, CompanionProtocol::TriState::Off) ? icon_mic_off_36 : icon_mic_36;
}

const freeink::Icon& cameraStatusIcon(uint8_t state) {
  return triStateIs(state, CompanionProtocol::TriState::Off) ? icon_video_off_36 : icon_video_36;
}

std::string pressedText(const char* label, uint16_t counter) {
  char text[32];
  snprintf(text, sizeof(text), "%s #%u pressed", label, static_cast<unsigned>(counter));
  return text;
}

const char* buttonLabel(uint8_t buttonId) {
  switch (static_cast<CompanionProtocol::ButtonId>(buttonId)) {
    case CompanionProtocol::ButtonId::ToggleMute:
      return "Mute";
    case CompanionProtocol::ButtonId::ToggleHand:
      return "Hand";
    case CompanionProtocol::ButtonId::ToggleCamera:
      return "Camera";
    default:
      return "Button";
  }
}

std::string acknowledgedText(const char* label, uint16_t counter, uint32_t latencyMs) {
  char text[48];
  snprintf(text, sizeof(text), "%s #%u roundtrip %lums", label, static_cast<unsigned>(counter),
           static_cast<unsigned long>(latencyMs));
  return text;
}

void drawStatusTile(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& tile, const freeink::Icon& icon,
                    const char* label, const char* value, bool active, bool locked = false) {
  const freeink::ui::Color ink = active ? freeink::ui::Color::White : freeink::ui::Color::Black;
  if (active) {
    target.fill(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  } else {
    target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
  }
  if (locked && tile.width > 12 && tile.height > 12) {
    target.stroke(tile.inset(freeink::ui::Insets{5, 5, 5, 5}),
                  freeink::ui::Paint::solid(active ? freeink::ui::Color::White : freeink::ui::Color::Black), 1, 3);
  }

  freeink::ui::TextStyle labelStyle;
  labelStyle.align = freeink::ui::TextAlign::Left;
  labelStyle.maxLines = 1;
  labelStyle.color = ink;

  freeink::ui::TextStyle valueStyle = labelStyle;

  drawIcon(target, freeink::ui::Rect{static_cast<int16_t>(tile.x + 14), static_cast<int16_t>(tile.y + 14), 36, 36},
           icon, ink);
  if (locked) {
    drawIcon(target,
             freeink::ui::Rect{static_cast<int16_t>(tile.right() - 44), static_cast<int16_t>(tile.y + 14), 28, 28},
             icon_lock_28, ink);
  }
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(tile.x + 62), static_cast<int16_t>(tile.y + 16),
                                          static_cast<int16_t>(tile.width - (locked ? 112 : 76)), 28},
                        label, labelStyle);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(tile.x + 62), static_cast<int16_t>(tile.y + 52),
                                          static_cast<int16_t>(tile.width - 76), 32},
                        value, valueStyle);
}

void drawConnectionTile(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& tile, bool connected,
                        bool subscribed, bool advertising) {
  const freeink::Icon& icon = connected ? icon_bluetooth_48 : icon_bluetooth_off_48;
  const freeink::ui::Color ink = connected ? freeink::ui::Color::White : freeink::ui::Color::Black;
  if (connected) {
    target.fill(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  } else {
    target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
  }

  freeink::ui::TextStyle label;
  label.align = freeink::ui::TextAlign::Center;
  label.maxLines = 1;
  label.color = ink;

  drawIcon(target,
           freeink::ui::Rect{static_cast<int16_t>(tile.x + (tile.width - 48) / 2), static_cast<int16_t>(tile.y + 12),
                             48, 48},
           icon, ink);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(tile.x + 8), static_cast<int16_t>(tile.y + 66),
                                          static_cast<int16_t>(tile.width - 16), 24},
                        connected ? (subscribed ? "Subscribed" : "Connected") : (advertising ? "Advertising" : "Idle"),
                        label);
}

void drawMeetingTile(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& tile, bool active,
                     const std::string& meetingName) {
  const freeink::ui::Color ink = active ? freeink::ui::Color::White : freeink::ui::Color::Black;
  if (active) {
    target.fill(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  } else {
    target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
  }

  freeink::ui::TextStyle text;
  text.align = freeink::ui::TextAlign::Center;
  text.maxLines = 2;
  text.color = ink;

  const char* label = active ? (meetingName.empty() ? "Meeting" : meetingName.c_str()) : "<no meeting>";
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(tile.x + 12), static_cast<int16_t>(tile.y + 26),
                                          static_cast<int16_t>(tile.width - 24), 54},
                        label, text);
}
}  // namespace

CompanionPage::CompanionPage(EInkDisplay& display) : Page(display) {}

PageId CompanionPage::id() const {
  return PageId::Companion;
}

const char* CompanionPage::name() const {
  return "companion";
}

void CompanionPage::onEnter() {
  CompanionBleService::getInstance().setStatusChangedCallback(requestCompanionRender);
  ensureStarted();
}

bool CompanionPage::handleButton(ButtonPressKind kind) {
  CompanionBleService& service = CompanionBleService::getInstance();
  ensureStarted();
  const CompanionBleService::HostStatus host = service.getHostStatus();
  uint16_t counter = 0;
  switch (kind) {
    case ButtonPressKind::Left:
      actionMessage_ = service.notifyToggleMuteReleased(&counter) ? pressedText("Mute", counter) : "Host not ready";
      return true;
    case ButtonPressKind::Right:
      if (host.handLocked) {
        actionMessage_ = "Hand locked";
        return true;
      }
      actionMessage_ = service.notifyToggleHandReleased(&counter) ? pressedText("Hand", counter) : "Host not ready";
      return true;
    case ButtonPressKind::Confirm:
      if (host.cameraLocked) {
        actionMessage_ = "Camera locked";
        return true;
      }
      actionMessage_ = service.notifyToggleCameraReleased(&counter) ? pressedText("Camera", counter) : "Host not ready";
      return true;
    default:
      return false;
  }
}

bool CompanionPage::handleTouch(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) {
  const freeink::ui::Rect content = pageMainPanel(target).inset(freeink::ui::Insets{12, 8, 24, 18});
  constexpr int16_t gap = 12;
  const int16_t tileW = static_cast<int16_t>((content.width - gap) / 2);
  const int16_t mediaTileY = static_cast<int16_t>(content.y + 188);

  // These are the visible action tiles: microphone, camera, then hand.
  if (freeink::ui::Rect{content.x, mediaTileY, tileW, 100}.contains(x, y)) {
    return handleButton(ButtonPressKind::Left);
  }
  if (freeink::ui::Rect{static_cast<int16_t>(content.x + tileW + gap), mediaTileY, tileW, 100}.contains(x, y)) {
    return handleButton(ButtonPressKind::Confirm);
  }
  if (freeink::ui::Rect{content.x, static_cast<int16_t>(mediaTileY + 124), content.width, 88}.contains(x, y)) {
    return handleButton(ButtonPressKind::Right);
  }
  return false;
}

void CompanionPage::onLeave() {
  CompanionBleService& service = CompanionBleService::getInstance();
  service.setStatusChangedCallback(nullptr);
}

std::unique_ptr<RenderTransaction> CompanionPage::render(freeink::ui::DisplayTarget& target,
                                                         EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  CompanionBleService& service = CompanionBleService::getInstance();
  const CompanionBleService::HostStatus host = service.getHostStatus();
  const CompanionBleService::PendingButtonStatus pending = service.getPendingButtonStatus();

  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;

  freeink::ui::TextStyle center;
  center.align = freeink::ui::TextAlign::Center;
  center.maxLines = 1;

  const bool connected = service.isHostConnected();
  const bool micLive = triStateIs(host.microphone, CompanionProtocol::TriState::On);
  const bool cameraLive = triStateIs(host.camera, CompanionProtocol::TriState::On);
  const bool handRaised = triStateIs(host.hand, CompanionProtocol::TriState::On);

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 20), content.width, 34},
                        "Laptop Companion", title);
  drawIcon(target, freeink::ui::Rect{static_cast<int16_t>(content.right() - 36), static_cast<int16_t>(content.y + 18),
                                     36, 36},
           icon_laptop_36);

  constexpr int16_t gap = 12;
  const int16_t tileY = static_cast<int16_t>(content.y + 66);
  const int16_t linkW = 154;
  const int16_t tileW = static_cast<int16_t>((content.width - gap) / 2);
  drawConnectionTile(target, freeink::ui::Rect{content.x, tileY, linkW, 96}, connected, service.isButtonSubscribed(),
                     service.isAdvertising());
  drawMeetingTile(target,
                  freeink::ui::Rect{static_cast<int16_t>(content.x + linkW + gap), tileY,
                                    static_cast<int16_t>(content.width - linkW - gap), 96},
                  host.meetingDetected, host.meetingName);

  const int16_t mediaTileY = static_cast<int16_t>(content.y + 188);
  drawStatusTile(target, freeink::ui::Rect{content.x, mediaTileY, tileW, 100}, micStatusIcon(host.microphone),
                 "Microphone", triStateText(host.microphone, "Muted", "Live"), micLive);
  drawStatusTile(target,
                 freeink::ui::Rect{static_cast<int16_t>(content.x + tileW + gap), mediaTileY, tileW, 100},
                 cameraStatusIcon(host.camera), "Camera",
                 host.cameraLocked ? "Locked" : triStateText(host.camera, "Off", "Active"), cameraLive,
                 host.cameraLocked);
  drawStatusTile(target, freeink::ui::Rect{content.x, static_cast<int16_t>(mediaTileY + 124), content.width, 88},
                 icon_activity_36, "Hand", host.handLocked ? "Locked" : triStateText(host.hand, "Lowered", "Raised"),
                 handRaised, host.handLocked);

  std::string pendingText;
  if (pending.mutePending) {
    pendingText = pressedText("Mute", pending.muteCounter);
  } else if (pending.handPending) {
    pendingText = pressedText("Hand", pending.handCounter);
  } else if (pending.cameraPending) {
    pendingText = pressedText("Camera", pending.cameraCounter);
  } else if (!actionMessage_.empty() && actionMessage_.find("pressed") != std::string::npos) {
    actionMessage_.clear();
  }

  if (pendingText.empty() && actionMessage_.empty() && pending.lastAcknowledgedValid) {
    pendingText = acknowledgedText(buttonLabel(pending.lastAcknowledgedButtonId), pending.lastAcknowledgedCounter,
                                   pending.lastAcknowledgedLatencyMs);
  }

  const std::string& message = pendingText.empty() ? actionMessage_ : pendingText;
  if (!message.empty()) {
    freeink::ui::drawText(target,
                          freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 72), content.width, 30},
                          message.c_str(), center);
  }

  drawPageChrome(target);
  return tx;
}

const char* CompanionPage::triStateText(uint8_t state, const char* offText, const char* onText) const {
  switch (state) {
    case static_cast<uint8_t>(CompanionProtocol::TriState::Off):
      return offText;
    case static_cast<uint8_t>(CompanionProtocol::TriState::On):
      return onText;
    default:
      return "Unknown";
  }
}

bool CompanionPage::ensureStarted() {
  CompanionBleService& service = CompanionBleService::getInstance();
  if (started_ && service.isRunning()) return true;
  started_ = service.begin();
  actionMessage_ = started_ ? "" : "BLE start failed";
  return started_;
}

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
    case CompanionProtocol::ButtonId::SwitchDesktop:
      return "Desktop";
    case CompanionProtocol::ButtonId::ReactLike:
      return "Like";
    case CompanionProtocol::ButtonId::ReactHeart:
      return "Heart";
    case CompanionProtocol::ButtonId::ReactApplause:
      return "Clap";
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

// A press is styled with a dithered gray fill until it settles, so the tile visibly
// acknowledges the tap without yet claiming the new state.
void fillPressed(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& tile) {
  target.fill(tile, freeink::ui::Paint::dither(freeink::ui::Color::LightGray), 4);
  target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
}

void drawStatusTile(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& tile, const freeink::Icon& icon,
                    const char* label, const char* value, bool active, bool locked = false, bool pending = false) {
  const freeink::ui::Color ink = (active && !pending) ? freeink::ui::Color::White : freeink::ui::Color::Black;
  if (pending) {
    fillPressed(target, tile);
  } else if (active) {
    target.fill(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  } else {
    target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
  }
  if (locked && tile.width > 12 && tile.height > 12) {
    target.stroke(tile.inset(freeink::ui::Insets{5, 5, 5, 5}), freeink::ui::Paint::solid(ink), 1, 3);
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
                     const std::string& meetingName) {  const freeink::ui::Color ink = active ? freeink::ui::Color::White : freeink::ui::Color::Black;
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

constexpr int16_t kDesktopRowHeight = 84;
constexpr int16_t kDesktopTileGap = 8;
constexpr int16_t kMessageHeight = 30;
constexpr int16_t kMessageGap = 8;

// Meeting reactions sit just under the hand tile. They fire once per tap and carry no
// state, so they are drawn as plain outlined buttons that never appear engaged.
constexpr int16_t kReactionRowTop = 412;
constexpr int16_t kReactionRowHeight = 86;
constexpr int16_t kReactionGap = 12;
constexpr uint8_t kReactionCount = 3;

struct ReactionButton {
  CompanionProtocol::ButtonId id;
  const freeink::Icon& icon;
  const char* label;
};

const ReactionButton kReactions[kReactionCount] = {
    {CompanionProtocol::ButtonId::ReactLike, icon_thumbs_up_28, "Like"},
    {CompanionProtocol::ButtonId::ReactHeart, icon_heart_28, "Heart"},
    {CompanionProtocol::ButtonId::ReactApplause, icon_clap_28, "Clap"},
};

freeink::ui::Rect reactionRowRect(const freeink::ui::Rect& content) {
  return freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + kReactionRowTop), content.width,
                           kReactionRowHeight};
}

freeink::ui::Rect reactionTileRect(const freeink::ui::Rect& row, uint8_t index) {
  const int16_t totalGap = static_cast<int16_t>(kReactionGap * (kReactionCount - 1));
  const int16_t tileW = static_cast<int16_t>((row.width - totalGap) / kReactionCount);
  return freeink::ui::Rect{static_cast<int16_t>(row.x + index * (tileW + kReactionGap)), row.y, tileW, row.height};
}

void drawReactionRow(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& content,
                     const CompanionBleService::PendingButtonStatus& pending) {
  const freeink::ui::Rect row = reactionRowRect(content);
  for (uint8_t i = 0; i < kReactionCount; ++i) {
    const freeink::ui::Rect tile = reactionTileRect(row, i);
    const bool pressed =
        pending.reactionPending && pending.reactionButtonId == static_cast<uint8_t>(kReactions[i].id);
    if (pressed) {
      fillPressed(target, tile);
    } else {
      target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
    }

    drawIcon(target,
             freeink::ui::Rect{static_cast<int16_t>(tile.x + (tile.width - 28) / 2),
                               static_cast<int16_t>(tile.y + 14), 28, 28},
             kReactions[i].icon);

    freeink::ui::TextStyle text;
    text.align = freeink::ui::TextAlign::Center;
    text.maxLines = 1;
    freeink::ui::drawText(target,
                          freeink::ui::Rect{static_cast<int16_t>(tile.x + 2), static_cast<int16_t>(tile.y + 48),
                                            static_cast<int16_t>(tile.width - 4), 24},
                          kReactions[i].label, text);
  }
}

// The desktop switcher owns the bottom strip; timings sit directly above it.
freeink::ui::Rect desktopRowRect(const freeink::ui::Rect& content) {
  return freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - kDesktopRowHeight), content.width,
                           kDesktopRowHeight};
}

freeink::ui::Rect messageRect(const freeink::ui::Rect& content) {
  const freeink::ui::Rect row = desktopRowRect(content);
  return freeink::ui::Rect{content.x, static_cast<int16_t>(row.y - kMessageGap - kMessageHeight), content.width,
                           kMessageHeight};
}

freeink::ui::Rect desktopTileRect(const freeink::ui::Rect& row, uint8_t count, uint8_t index) {
  if (count == 0 || index >= count) return freeink::ui::Rect{};
  const int16_t totalGap = static_cast<int16_t>(kDesktopTileGap * (count - 1));
  const int16_t tileW = static_cast<int16_t>((row.width - totalGap) / count);
  return freeink::ui::Rect{static_cast<int16_t>(row.x + index * (tileW + kDesktopTileGap)), row.y, tileW, row.height};
}

// Outlines a tile with short dashes, marking a remote desktop that has no live session.
void strokeDashedRect(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& rect, freeink::ui::Color color,
                      uint8_t width, int16_t dash, int16_t gap) {
  const freeink::ui::Paint paint = freeink::ui::Paint::solid(color);
  const int16_t step = static_cast<int16_t>(dash + gap);
  for (int16_t x = rect.x; x < rect.right(); x = static_cast<int16_t>(x + step)) {
    const int16_t end = static_cast<int16_t>(x + dash > rect.right() ? rect.right() : x + dash);
    target.line(freeink::ui::Point{x, rect.y}, freeink::ui::Point{end, rect.y}, width, paint);
    target.line(freeink::ui::Point{x, static_cast<int16_t>(rect.bottom() - 1)},
                freeink::ui::Point{end, static_cast<int16_t>(rect.bottom() - 1)}, width, paint);
  }
  for (int16_t y = rect.y; y < rect.bottom(); y = static_cast<int16_t>(y + step)) {
    const int16_t end = static_cast<int16_t>(y + dash > rect.bottom() ? rect.bottom() : y + dash);
    target.line(freeink::ui::Point{rect.x, y}, freeink::ui::Point{rect.x, end}, width, paint);
    target.line(freeink::ui::Point{static_cast<int16_t>(rect.right() - 1), y},
                freeink::ui::Point{static_cast<int16_t>(rect.right() - 1), end}, width, paint);
  }
}

void drawDesktopTile(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& tile, const std::string& name,
                     bool remote, bool disconnected, bool active, bool pending) {
  const freeink::ui::Color ink = (active && !pending) ? freeink::ui::Color::White : freeink::ui::Color::Black;
  if (pending) {
    fillPressed(target, tile);
  } else if (active) {
    target.fill(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  } else if (disconnected) {
    strokeDashedRect(target, tile, freeink::ui::Color::Black, 1, 6, 4);
  } else {
    target.stroke(tile, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
  }

  const freeink::ui::Rect iconRect{static_cast<int16_t>(tile.x + (tile.width - 28) / 2),
                                   static_cast<int16_t>(tile.y + 14), 28, 28};
  drawIcon(target, iconRect, remote ? icon_radio_tower_28 : icon_monitor_28, ink);
  if (disconnected) {
    // Strike through the icon so an offline desktop reads as unavailable at a glance.
    target.line(freeink::ui::Point{static_cast<int16_t>(iconRect.x + 2), static_cast<int16_t>(iconRect.bottom() - 3)},
                freeink::ui::Point{static_cast<int16_t>(iconRect.right() - 3), static_cast<int16_t>(iconRect.y + 2)}, 2,
                freeink::ui::Paint::solid(ink));
  }

  if (name.empty()) return;

  freeink::ui::TextStyle text;
  text.align = freeink::ui::TextAlign::Center;
  text.maxLines = 1;
  text.color = ink;

  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(tile.x + 2), static_cast<int16_t>(tile.y + 48),
                                          static_cast<int16_t>(tile.width - 4), 24},
                        name.c_str(), text);
}

void drawDesktopRow(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& content,
                    const CompanionBleService::DesktopStatus& desktops,
                    const CompanionBleService::PendingButtonStatus& pending) {
  const freeink::ui::Rect row = desktopRowRect(content);
  if (!desktops.valid || desktops.count == 0) {
    target.stroke(row, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
    freeink::ui::TextStyle text;
    text.align = freeink::ui::TextAlign::Center;
    text.maxLines = 1;
    freeink::ui::drawText(target,
                          freeink::ui::Rect{row.x, static_cast<int16_t>(row.y + (row.height - 24) / 2), row.width, 24},
                          "No desktop info", text);
    return;
  }

  for (uint8_t i = 0; i < desktops.count; ++i) {
    const bool pendingTile = pending.desktopPending && pending.desktopTargetIndex == i;
    drawDesktopTile(target, desktopTileRect(row, desktops.count, i), desktops.desktops[i].name,
                    desktops.desktops[i].remote, desktops.desktops[i].disconnected, desktops.activeIndex == i,
                    pendingTile);
  }
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
  if (handleReactionTouch(content, x, y)) {
    return true;
  }
  return handleDesktopTouch(content, x, y);
}

bool CompanionPage::handleReactionTouch(const freeink::ui::Rect& content, int16_t x, int16_t y) {
  const freeink::ui::Rect row = reactionRowRect(content);
  if (!row.contains(x, y)) return false;

  CompanionBleService& service = CompanionBleService::getInstance();
  for (uint8_t i = 0; i < kReactionCount; ++i) {
    if (!reactionTileRect(row, i).contains(x, y)) continue;

    uint16_t counter = 0;
    actionMessage_ = service.notifyReactionReleased(kReactions[i].id, &counter)
                         ? pressedText(kReactions[i].label, counter)
                         : "Host not ready";
    return true;
  }
  return true;
}

bool CompanionPage::handleDesktopTouch(const freeink::ui::Rect& content, int16_t x, int16_t y) {
  CompanionBleService& service = CompanionBleService::getInstance();
  const CompanionBleService::DesktopStatus desktops = service.getDesktopStatus();
  const freeink::ui::Rect row = desktopRowRect(content);
  if (!row.contains(x, y)) return false;
  if (!desktops.valid || desktops.count == 0) {
    actionMessage_ = "No desktop info";
    return true;
  }

  for (uint8_t i = 0; i < desktops.count; ++i) {
    if (!desktopTileRect(row, desktops.count, i).contains(x, y)) continue;
    if (desktops.activeIndex == i) {
      actionMessage_ = "Desktop already active";
      return true;
    }
    if (desktops.desktops[i].disconnected) {
      // Connecting a remote machine has to be done from Task view; the tile becomes
      // switchable here once its session is up.
      actionMessage_ = "Offline - connect in Task view";
      return true;
    }

    uint16_t counter = 0;
    actionMessage_ = service.notifySwitchDesktopReleased(i, &counter) ? pressedText("Desktop", counter)
                                                                     : "Desktop switch unavailable";
    return true;
  }
  return true;
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
                 "Microphone", triStateText(host.microphone, "Muted", "Live"), micLive, false, pending.mutePending);
  drawStatusTile(target,
                 freeink::ui::Rect{static_cast<int16_t>(content.x + tileW + gap), mediaTileY, tileW, 100},
                 cameraStatusIcon(host.camera), "Camera",
                 host.cameraLocked ? "Locked" : triStateText(host.camera, "Off", "Active"), cameraLive,
                 host.cameraLocked, pending.cameraPending);
  drawStatusTile(target, freeink::ui::Rect{content.x, static_cast<int16_t>(mediaTileY + 124), content.width, 88},
                 icon_activity_36, "Hand", host.handLocked ? "Locked" : triStateText(host.hand, "Lowered", "Raised"),
                 handRaised, host.handLocked, pending.handPending);

  std::string pendingText;
  if (pending.mutePending) {
    pendingText = pressedText("Mute", pending.muteCounter);
  } else if (pending.handPending) {
    pendingText = pressedText("Hand", pending.handCounter);
  } else if (pending.cameraPending) {
    pendingText = pressedText("Camera", pending.cameraCounter);
  } else if (pending.desktopPending) {
    pendingText = pressedText("Desktop", pending.desktopCounter);
  } else if (!actionMessage_.empty() && actionMessage_.find("pressed") != std::string::npos) {
    actionMessage_.clear();
  }

  if (pendingText.empty() && actionMessage_.empty() && pending.lastAcknowledgedValid) {
    pendingText = acknowledgedText(buttonLabel(pending.lastAcknowledgedButtonId), pending.lastAcknowledgedCounter,
                                   pending.lastAcknowledgedLatencyMs);
  }

  const std::string& message = pendingText.empty() ? actionMessage_ : pendingText;
  if (!message.empty()) {
    freeink::ui::drawText(target, messageRect(content), message.c_str(), center);
  }

  drawDesktopRow(target, content, service.getDesktopStatus(), pending);
  drawReactionRow(target, content, pending);

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

#include "CompanionStatsPage.h"

#include "DisplayWorker.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "Settings.h"
#include "companion/CompanionBleService.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

namespace {
const char* triStateText(uint8_t value, const char* falseText, const char* trueText) {
  if (value == 2) return trueText;
  if (value == 1) return falseText;
  return "unknown";
}

void requestCompanionStatsRender() {
  if (!displayWorkerReady()) return;
  requestRender(RenderKind::ActivePage, refreshModeFromSettings(copySettings()));
}
}  // namespace

CompanionStatsPage::CompanionStatsPage(EInkDisplay& display) : Page(display) {}

PageId CompanionStatsPage::id() const {
  return PageId::CompanionStats;
}

const char* CompanionStatsPage::name() const {
  return "companion-stats";
}

void CompanionStatsPage::onEnter() {
  CompanionBleService& service = CompanionBleService::getInstance();
  service.setStatusChangedCallback(requestCompanionStatsRender);
  service.begin();
}

void CompanionStatsPage::onLeave() {
  CompanionBleService::getInstance().setStatusChangedCallback(nullptr);
}

std::unique_ptr<RenderTransaction> CompanionStatsPage::render(freeink::ui::DisplayTarget& target,
                                                              EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);

  CompanionBleService& service = CompanionBleService::getInstance();
  const CompanionBleService::HostStatus host = service.getHostStatus();
  const CompanionBleService::ActivityStats stats = service.getActivityStats();
  const uint32_t totalRenderRequests = renderRequestCount();
  const uint32_t sessionRenderRequests = service.getBluetoothSessionRenderRequests();
  const std::string timing = service.formatTimingDiagnostics();
  const std::string activity = service.formatActivityDeltaDiagnostics();

  const freeink::ui::Rect panel = pageMainPanel(target);
  const freeink::ui::Rect content = panel.inset(freeink::ui::Insets{24, 8, 24, 18});
  target.fill(panel, freeink::ui::Paint::solid(freeink::ui::Color::White));

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 1;

  char statusLine[112];
  snprintf(statusLine, sizeof(statusLine), "Status: %s", service.getStatusText().c_str());
  char linkLine[112];
  snprintf(linkLine, sizeof(linkLine), "Host:%s  Advertising:%s  Subscribed:%s",
           service.isHostConnected() ? "connected" : "not connected", service.isAdvertising() ? "yes" : "no",
           service.isButtonSubscribed() ? "yes" : "no");
  char gapLine[112];
  snprintf(gapLine, sizeof(gapLine), "GAP connect:%lu disconnect:%lu param req:%lu update:%lu",
           static_cast<unsigned long>(stats.gapConnects), static_cast<unsigned long>(stats.gapDisconnects),
           static_cast<unsigned long>(stats.connParamRequests), static_cast<unsigned long>(stats.connParamUpdates));
  char hostLine[112];
  snprintf(hostLine, sizeof(hostLine), "Host wr:%lu chg:%lu btn sub/ntf:%lu/%lu part sub/ntf:%lu/%lu",
           static_cast<unsigned long>(stats.hostWrites), static_cast<unsigned long>(stats.hostStateChanges),
           static_cast<unsigned long>(stats.buttonSubscribes),
           static_cast<unsigned long>(stats.buttonNotifications),
           static_cast<unsigned long>(stats.participationSubscribes),
           static_cast<unsigned long>(stats.participationNotifications));
  char workerLine[112];
  snprintf(workerLine, sizeof(workerLine), "Worker upd:%lu m:%lu adv:%lu renders:%lu bluetooth_session:%lu",
           static_cast<unsigned long>(stats.updateCalls), static_cast<unsigned long>(stats.maintenanceRuns),
            static_cast<unsigned long>(stats.advertisingRestarts), static_cast<unsigned long>(totalRenderRequests),
            static_cast<unsigned long>(sessionRenderRequests));
  char meetingLine[128];
  snprintf(meetingLine, sizeof(meetingLine), "Meeting:%s  Name:%s", host.meetingDetected ? "active" : "none",
           host.meetingName.empty() ? "--" : host.meetingName.c_str());
  char mediaLine[112];
  snprintf(mediaLine, sizeof(mediaLine), "Mic:%s #%u  Camera:%s #%u  Hand:%s #%u",
           triStateText(host.microphone, "muted", "live"), static_cast<unsigned>(host.microphoneCounter),
           triStateText(host.camera, "off", "on"), static_cast<unsigned>(host.cameraCounter),
           triStateText(host.hand, "lowered", "raised"), static_cast<unsigned>(host.handCounter));
  char hostMessageLine[128];
  snprintf(hostMessageLine, sizeof(hostMessageLine), "Host text: %s", host.message.empty() ? "--" : host.message.c_str());

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, content.y, content.width, 34}, "Companion Stats", title);
  int16_t y = static_cast<int16_t>(content.y + 52);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, statusLine, body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, linkLine, body);
  y += 44;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, meetingLine, body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, mediaLine, body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, hostMessageLine, body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, gapLine, body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, hostLine, body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, workerLine, body);
  y += 44;

  const size_t timingBreak = timing.find('\n');
  const std::string timingA = timingBreak == std::string::npos ? timing : timing.substr(0, timingBreak);
  const std::string timingB = timingBreak == std::string::npos ? "" : timing.substr(timingBreak + 1);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, timingA.c_str(), body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, timingB.c_str(), body);
  y += 34;
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 28}, activity.c_str(), body);

  drawPageChrome(target);
  return tx;
}

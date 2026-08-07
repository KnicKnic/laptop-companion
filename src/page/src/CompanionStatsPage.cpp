#include "CompanionStatsPage.h"

#include "DisplayWorker.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "PowerStats.h"
#include "Settings.h"
#include "companion/CompanionBleService.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIFontSmall.h>

namespace {
constexpr freeink::ui::FontId kStatsFontSlot = 3;
constexpr int16_t kStatsLineGap = 2;

const char* triStateText(uint8_t value, const char* falseText, const char* trueText) {
  if (value == 2) return trueText;
  if (value == 1) return falseText;
  return "unknown";
}

void requestCompanionStatsRender() {
  if (!displayWorkerReady()) return;
  requestRender(RenderKind::ActivePage, refreshModeFromSettings(copySettings()));
}

int16_t wrappedTextHeight(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& content, const char* text,
                          const freeink::ui::TextStyle& style) {
  const int16_t lineHeight = target.lineHeight(style.font);
  const freeink::ui::Size measured = freeink::ui::measureWrappedText(target, text, style, content.width);
  return measured.height > lineHeight ? measured.height : lineHeight;
}

void drawStatsRows(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& content, int16_t y, int16_t bottom,
                   const char* const* rows, uint8_t rowCount, uint8_t firstRow,
                   const freeink::ui::TextStyle& style) {
  for (uint8_t i = firstRow; i < rowCount; ++i) {
    const int16_t textHeight = wrappedTextHeight(target, content, rows[i], style);
    if (static_cast<int16_t>(y + textHeight) > bottom) return;
    freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, textHeight}, rows[i], style);
    y = static_cast<int16_t>(y + textHeight + kStatsLineGap);
  }
}

void clampScroll(uint8_t& offset, uint8_t rowCount) {
  if (rowCount == 0) {
    offset = 0;
  } else if (offset >= rowCount) {
    offset = static_cast<uint8_t>(rowCount - 1);
  }
}
}  // namespace

CompanionStatsPage::CompanionStatsPage(EInkDisplay& display) : Page(display) {}

PageId CompanionStatsPage::id() const {
  return PageId::CompanionStats;
}

const char* CompanionStatsPage::name() const {
  return "companion-stats";
}

bool CompanionStatsPage::handleButton(ButtonPressKind kind) {
  switch (kind) {
    case ButtonPressKind::Up:
      if (scrollOffset_ > 0) --scrollOffset_;
      return true;
    case ButtonPressKind::Down:
      if (scrollOffset_ < 255) ++scrollOffset_;
      return true;
    default:
      return false;
  }
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
  target.setFont(kStatsFontSlot, freeink::ui::kNotoSansSmallFont);

  CompanionBleService& service = CompanionBleService::getInstance();
  const CompanionBleService::HostStatus host = service.getHostStatus();
  const CompanionBleService::ActivityStats stats = service.getActivityStats();
  const uint32_t totalRenderRequests = renderRequestCount();
  const uint32_t sessionRenderRequests = service.getBluetoothSessionRenderRequests();
  const std::string timing = service.formatTimingDiagnostics();
  const std::string activity = service.formatActivityDeltaDiagnostics();
  const PowerStatsSnapshot power = copyPowerStats();
  const std::string powerTotal = formatPowerStatsTotalLine(power);
  const std::string powerDelta = formatPowerStatsDeltaLine(power);
  const std::string powerAccounting = formatPowerStatsAccountingLine(power);
  std::string cpuMaxLine;
  std::string apbMaxLine;
  std::string dfsLine;
  formatPowerStatsFrequencyLines(power, cpuMaxLine, apbMaxLine, dfsLine);
  std::string powerWakeLines[POWER_STATS_WAKE_CAUSE_COUNT];
  const uint8_t powerWakeLineCount =
      formatPowerStatsWakeDeltaLines(power, powerWakeLines, POWER_STATS_WAKE_CAUSE_COUNT);
  const std::string timerLine = formatEspTimerActivity();
  const std::string alarmLine = formatEspTimerAlarmLine();
  const std::string btLockLine = formatBtLockTraceDiagnostics();
  std::string pmLock1;
  std::string pmLock2;
  std::string pmLock3;
  std::string pmLock4;
  std::string pmLock5;
  std::string pmLock6;
  std::string pmLock7;
  formatPmLockActivity(pmLock1, pmLock2, pmLock3, pmLock4, pmLock5, pmLock6, pmLock7);
  std::string taskLines[10];
  const uint8_t taskLineCount = formatTaskActivity(taskLines, 10);

  const freeink::ui::Rect panel = pageMainPanel(target);
  const freeink::ui::Rect content = panel.inset(freeink::ui::Insets{24, 8, 24, 18});
  target.fill(panel, freeink::ui::Paint::solid(freeink::ui::Color::White));

  freeink::ui::TextStyle title;
  title.font = freeink::ui::FONT_SLOT_BODY;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.font = kStatsFontSlot;
  body.maxLines = 2;

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
  snprintf(workerLine, sizeof(workerLine), "Worker updates:%lu maintenance:%lu adv restarts:%lu",
           static_cast<unsigned long>(stats.updateCalls), static_cast<unsigned long>(stats.maintenanceRuns),
           static_cast<unsigned long>(stats.advertisingRestarts));
  char renderLine[112];
  snprintf(renderLine, sizeof(renderLine), "renders: %lu session: %lu",
           static_cast<unsigned long>(totalRenderRequests), static_cast<unsigned long>(sessionRenderRequests));
  char meetingLine[128];
  snprintf(meetingLine, sizeof(meetingLine), "Meeting:%s  Name:%s", host.meetingDetected ? "active" : "none",
           host.meetingName.empty() ? "--" : host.meetingName.c_str());
  char mediaLine[112];
  snprintf(mediaLine, sizeof(mediaLine), "Mic:%s #%u  Camera:%s%s #%u  Hand:%s%s #%u",
           triStateText(host.microphone, "muted", "live"), static_cast<unsigned>(host.microphoneCounter),
           triStateText(host.camera, "off", "on"), host.cameraLocked ? "(locked)" : "",
           static_cast<unsigned>(host.cameraCounter),
           triStateText(host.hand, "lowered", "raised"), host.handLocked ? "(locked)" : "",
           static_cast<unsigned>(host.handCounter));
  char hostMessageLine[128];
  snprintf(hostMessageLine, sizeof(hostMessageLine), "Host text: %s", host.message.empty() ? "--" : host.message.c_str());
  const size_t timingBreak = timing.find('\n');
  const std::string timingA = timingBreak == std::string::npos ? timing : timing.substr(0, timingBreak);
  const std::string timingB = timingBreak == std::string::npos ? "" : timing.substr(timingBreak + 1);
  char wakeBitsLine[96];
  snprintf(wakeBitsLine, sizeof(wakeBitsLine), "Last wake mask: 0x%08lX; unmapped: 0x%08lX",
           static_cast<unsigned long>(power.lastWakeCauseBits),
           static_cast<unsigned long>(power.unmappedWakeCauseBits));
  const char* rows[64] = {};
  uint8_t rowCount = 0;
  auto addRow = [&rows, &rowCount](const char* row) {
    if (rowCount < 64) rows[rowCount++] = row;
  };
  addRow(statusLine);
  addRow(linkLine);
  addRow(meetingLine);
  addRow(mediaLine);
  addRow(hostMessageLine);
  addRow(gapLine);
  addRow(hostLine);
  addRow(workerLine);
  addRow(renderLine);
  addRow(timingA.c_str());
  addRow(timingB.c_str());
  addRow(activity.c_str());
  addRow(powerTotal.c_str());
  addRow(powerDelta.c_str());
  addRow(powerAccounting.c_str());
  addRow(cpuMaxLine.c_str());
  addRow(apbMaxLine.c_str());
  addRow(dfsLine.c_str());
  // Wake-cause detail is still collected, but hidden from the companion stats page for now.
  // for (uint8_t i = 0; i < powerWakeLineCount; ++i) addRow(powerWakeLines[i].c_str());
  addRow(wakeBitsLine);
  addRow(timerLine.c_str());
  addRow(alarmLine.c_str());
  addRow(btLockLine.c_str());
  addRow(pmLock1.c_str());
  addRow(pmLock2.c_str());
  addRow(pmLock3.c_str());
  addRow(pmLock4.c_str());
  addRow(pmLock5.c_str());
  addRow(pmLock6.c_str());
  addRow(pmLock7.c_str());
  for (uint8_t i = 0; i < taskLineCount; i++) addRow(taskLines[i].c_str());
  clampScroll(scrollOffset_, rowCount);

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, content.y, content.width, 34}, "Companion Stats", title);
  const int16_t y = static_cast<int16_t>(content.y + 42);
  const int16_t bottom = static_cast<int16_t>(content.bottom() - 30);
  drawStatsRows(target, content, y, bottom, rows, rowCount, scrollOffset_, body);
  char scrollLine[64];
  snprintf(scrollLine, sizeof(scrollLine), "Up/Down scroll  %u/%u", static_cast<unsigned>(scrollOffset_ + 1),
           static_cast<unsigned>(rowCount));
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 26), content.width, 22},
                        scrollLine, body);

  drawPageChrome(target);
  return tx;
}

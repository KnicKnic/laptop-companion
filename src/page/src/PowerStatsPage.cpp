#include "PowerStatsPage.h"

#include "PageDrawing.h"
#include "PageManager.h"
#include "PowerStats.h"
#include "Settings.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIFontSmall.h>

namespace {
constexpr freeink::ui::FontId kStatsFontSlot = 3;
constexpr int16_t kStatsLineGap = 2;

const char* displayDriverName() {
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "Lgfx EPD";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
  }
  return "unknown";
}

int16_t wrappedTextHeight(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& content, const char* text,
                          const freeink::ui::TextStyle& style) {
  const int16_t lineHeight = target.lineHeight(style.font);
  const freeink::ui::Size measured = freeink::ui::measureWrappedText(target, text, style, content.width);
  return measured.height > lineHeight ? measured.height : lineHeight;
}

void drawPowerRows(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& content, int16_t y,
                   int16_t bottom, const char* const* rows, uint8_t rowCount, uint8_t firstRow,
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

PowerStatsPage::PowerStatsPage(EInkDisplay& display) : Page(display) {}

PageId PowerStatsPage::id() const {
  return PageId::PowerStats;
}

const char* PowerStatsPage::name() const {
  return "power-stats";
}

bool PowerStatsPage::visible() const {
  return copySettings().system.debug.showPowerStatsPage;
}

bool PowerStatsPage::handleButton(ButtonPressKind kind) {
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

std::unique_ptr<RenderTransaction> PowerStatsPage::render(freeink::ui::DisplayTarget& target,
                                                          EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  target.setFont(kStatsFontSlot, freeink::ui::kNotoSansSmallFont);
  const PowerStatsSnapshot power = copyPowerStats();
  BatteryMonitor battery;
  const BatteryMonitor::Status batteryStatus = battery.readStatus();
  uint16_t checkedSoc = 0;
  const bool checkedSocKnown = battery.readPercentageChecked(checkedSoc);
  const uint16_t directMillivolts = battery.readMillivolts();
  const auto& gauge = BoardConfig::ACTIVE.batteryGauge;
  const std::string totalLine = formatPowerStatsTotalLine(power);
  const std::string deltaLine = formatPowerStatsDeltaLine(power);
  const std::string accountingLine = formatPowerStatsAccountingLine(power);
  std::string wakeLines[POWER_STATS_WAKE_CAUSE_COUNT];
  const uint8_t wakeLineCount = formatPowerStatsWakeDeltaLines(power, wakeLines, POWER_STATS_WAKE_CAUSE_COUNT);
  const std::string timerLine = formatEspTimerActivity();
  const std::string alarmLine = formatEspTimerAlarmLine();
  std::string pmLock1;
  std::string pmLock2;
  std::string pmLock3;
  std::string pmLock4;
  std::string pmLock5;
  formatPmLockActivity(pmLock1, pmLock2, pmLock3, pmLock4, pmLock5);
  std::string taskLines[10];
  const uint8_t taskLineCount = formatTaskActivity(taskLines, 10);

  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.font = freeink::ui::FONT_SLOT_BODY;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.font = kStatsFontSlot;
  body.maxLines = 2;

  char pmLine[96];
  snprintf(pmLine, sizeof(pmLine), "PM:%s Auto sleep:%s Profile:%s",
           !power.pmEnabledBySettings ? "off" : (power.pmConfigured ? "on" : esp_err_to_name(power.pmConfigResult)),
           power.autoLightSleep ? "on" : "off", power.pmProfilingAvailable ? "on" : "off");
  char displayDriverLine[64];
  snprintf(displayDriverLine, sizeof(displayDriverLine), "Display driver: %s", displayDriverName());
  char batteryBusLine[112];
  snprintf(batteryBusLine, sizeof(batteryBusLine), "Battery I2C: 0x%02X Wire%u SDA%d SCL%d", gauge.gaugeAddr,
           static_cast<unsigned>(gauge.i2cBus), gauge.i2cSda, gauge.i2cScl);
  char batteryStatusLine[128];
  snprintf(batteryStatusLine, sizeof(batteryStatusLine), "Battery status: supported:%u soc:%u/%u mv:%u/%u",
           batteryStatus.supported, batteryStatus.percentage, batteryStatus.percentageKnown,
           batteryStatus.millivolts, batteryStatus.millivoltsKnown);
  char batteryDirectLine[112];
  snprintf(batteryDirectLine, sizeof(batteryDirectLine), "Battery direct: checked SoC:%u/%u read mV:%u",
           checkedSoc, checkedSocKnown, directMillivolts);
  char rangeLine[96];
  snprintf(rangeLine, sizeof(rangeLine), "DFS range: %d -> %d MHz", power.maxFreqMhz, power.minFreqMhz);
  char cpuMaxDuration[32];
  char apbMaxDuration[32];
  char dfsDuration[32];
  formatDuration(cpuMaxDuration, sizeof(cpuMaxDuration), power.cpuMaxUs);
  formatDuration(apbMaxDuration, sizeof(apbMaxDuration), power.apbMaxUs);
  formatDuration(dfsDuration, sizeof(dfsDuration), power.dfsAwakeUs);
  char cpuMaxLine[96];
  snprintf(cpuMaxLine, sizeof(cpuMaxLine), "CPU max 240 MHz: %s  %lu%%", cpuMaxDuration,
           static_cast<unsigned long>(percentOf(power.cpuMaxUs, power.uptimeUs)));
  char apbMaxLine[96];
  snprintf(apbMaxLine, sizeof(apbMaxLine), "APB max 80 MHz: %s  %lu%%", apbMaxDuration,
           static_cast<unsigned long>(percentOf(power.apbMaxUs, power.uptimeUs)));
  char dfsLine[96];
  snprintf(dfsLine, sizeof(dfsLine), "DFS awake 10 MHz: %s  %lu%%", dfsDuration,
           static_cast<unsigned long>(percentOf(power.dfsAwakeUs, power.uptimeUs)));
  char renderLine[96];
  snprintf(renderLine, sizeof(renderLine), "Render requests:%lu", static_cast<unsigned long>(power.renderRequests));
  char rejectLine[96];
  snprintf(rejectLine, sizeof(rejectLine), "Light sleep rejects:%llu",
           static_cast<unsigned long long>(power.lightSleepRejects));
  char wakeBitsLine[96];
  snprintf(wakeBitsLine, sizeof(wakeBitsLine), "Last wake mask: 0x%08lX (bit0=undefined)",
           static_cast<unsigned long>(power.lastWakeCauseBits));
  char unmappedWakeBitsLine[96];
  snprintf(unmappedWakeBitsLine, sizeof(unmappedWakeBitsLine), "Unmapped wake-mask bits: 0x%08lX",
           static_cast<unsigned long>(power.unmappedWakeCauseBits));
  const char* rows[64] = {};
  uint8_t rowCount = 0;
  auto addRow = [&rows, &rowCount](const char* row) {
    if (rowCount < 64) rows[rowCount++] = row;
  };
  addRow(pmLine);
  addRow(displayDriverLine);
  addRow(batteryBusLine);
  addRow(batteryStatusLine);
  addRow(batteryDirectLine);
  addRow(rangeLine);
  addRow(totalLine.c_str());
  addRow(deltaLine.c_str());
  addRow(accountingLine.c_str());
  // Keep collecting wake-source diagnostics, but do not show them while the
  // source attribution is still being investigated.
  // for (uint8_t i = 0; i < wakeLineCount; ++i) addRow(wakeLines[i].c_str());
  // addRow(wakeBitsLine);
  // addRow(unmappedWakeBitsLine);
  addRow(rejectLine);
  addRow(cpuMaxLine);
  addRow(apbMaxLine);
  addRow(dfsLine);
  addRow(renderLine);
  addRow(timerLine.c_str());
  addRow(alarmLine.c_str());
  addRow(pmLock1.c_str());
  addRow(pmLock2.c_str());
  addRow(pmLock3.c_str());
  addRow(pmLock4.c_str());
  addRow(pmLock5.c_str());
  for (uint8_t i = 0; i < taskLineCount; i++) addRow(taskLines[i].c_str());
  clampScroll(scrollOffset_, rowCount);

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 8), content.width, 34},
                        "Power Stats", title);
  const int16_t y = static_cast<int16_t>(content.y + 44);
  const int16_t bottom = static_cast<int16_t>(content.bottom() - 38);
  drawPowerRows(target, content, y, bottom, rows, rowCount, scrollOffset_, body);
  char scrollLine[64];
  snprintf(scrollLine, sizeof(scrollLine), "Up/Down scroll  %u/%u", static_cast<unsigned>(scrollOffset_ + 1),
           static_cast<unsigned>(rowCount));
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 34), content.width, 24},
                        scrollLine, body);
  drawPageChrome(target);
  return tx;
}

#include "PowerStatsPage.h"

#include "PageDrawing.h"
#include "PageManager.h"
#include "PowerStats.h"
#include "Settings.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

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

std::unique_ptr<RenderTransaction> PowerStatsPage::render(freeink::ui::DisplayTarget& target,
                                                          EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  const PowerStatsSnapshot power = copyPowerStats();

  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 2;

  char uptime[32];
  char slept[32];
  formatDuration(uptime, sizeof(uptime), power.uptimeUs);
  formatDuration(slept, sizeof(slept), power.lightSleepUs);

  const uint64_t activeUs =
      power.freq10MhzUs + power.freq40MhzUs + power.freq80MhzUs + power.freq160MhzUs + power.freqOtherUs;

  char pmLine[96];
  snprintf(pmLine, sizeof(pmLine), "PM:%s Auto sleep:%s Profile:%s",
           !power.pmEnabledBySettings ? "off" : (power.pmConfigured ? "on" : esp_err_to_name(power.pmConfigResult)),
           power.autoLightSleep ? "on" : "off", power.pmProfilingAvailable ? "on" : "off");
  char rangeLine[96];
  snprintf(rangeLine, sizeof(rangeLine), "DFS range: %d -> %d MHz", power.maxFreqMhz, power.minFreqMhz);
  char sleepLine[96];
  snprintf(sleepLine, sizeof(sleepLine), "Light sleep: %s  (%llu ok / %llu reject)", slept,
           static_cast<unsigned long long>(power.lightSleepEntries),
           static_cast<unsigned long long>(power.lightSleepRejects));
  char uptimeLine[96];
  snprintf(uptimeLine, sizeof(uptimeLine), "PM profiled time: %s", uptime);
  char maxDuration[32];
  formatDuration(maxDuration, sizeof(maxDuration), power.freq160MhzUs);
  char maxLine[96];
  snprintf(maxLine, sizeof(maxLine), "160 MHz: %s  %lu%%", maxDuration,
           static_cast<unsigned long>(percentOf(power.freq160MhzUs, activeUs)));
  char freqLine1[96];
  snprintf(freqLine1, sizeof(freqLine1), "80 MHz:%lu%%  40 MHz:%lu%%  10 MHz:%lu%%",
           static_cast<unsigned long>(percentOf(power.freq80MhzUs, activeUs)),
           static_cast<unsigned long>(percentOf(power.freq40MhzUs, activeUs)),
           static_cast<unsigned long>(percentOf(power.freq10MhzUs, activeUs)));
  char freqLine2[96];
  snprintf(freqLine2, sizeof(freqLine2), "Other active freq: %lu%%",
           static_cast<unsigned long>(percentOf(power.freqOtherUs, activeUs)));

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 20), content.width, 44},
                        "Power Stats", title);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 98), content.width, 40},
                        pmLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 148), content.width, 40},
                        rangeLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 208), content.width, 40},
                        uptimeLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 258), content.width, 40},
                        sleepLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 318), content.width, 40},
                        maxLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 368), content.width, 40},
                        freqLine1, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 418), content.width, 40},
                        freqLine2, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 34), content.width, 24},
                        "/other/power-stats", body);
  drawPageChrome(target);
  return tx;
}

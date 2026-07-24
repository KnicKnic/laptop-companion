#include "CompanionSettingsWarningPage.h"

#include "AppState.h"
#include "PageDrawing.h"
#include "PageManager.h"
#include "Settings.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

CompanionSettingsWarningPage::CompanionSettingsWarningPage(EInkDisplay& display) : Page(display) {}

PageId CompanionSettingsWarningPage::id() const {
  return PageId::CompanionSettingsWarning;
}

const char* CompanionSettingsWarningPage::name() const {
  return "settings-warning";
}

std::unique_ptr<RenderTransaction> CompanionSettingsWarningPage::render(freeink::ui::DisplayTarget& target,
                                                                        EInkDisplay::RefreshMode mode,
                                                                        bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  const AppState state = copyAppState();

  const freeink::ui::Rect content = getPagePanel(target);
  const freeink::ui::Rect badge{static_cast<int16_t>(content.x + 28), static_cast<int16_t>(content.y + 34), 88, 88};

  target.fill(badge, freeink::ui::Paint::solid(freeink::ui::Color::Black), 6);
  target.fill(badge.inset(freeink::ui::Insets{12, 12, 12, 12}), freeink::ui::Paint::solid(freeink::ui::Color::White), 4);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 3;

  freeink::ui::TextStyle bang = title;
  bang.maxLines = 1;

  const char* sdLine = state.sdReady ? "SD card mounted." : "SD card is not available.";
  char settingsLine[96];
  snprintf(settingsLine, sizeof(settingsLine), "Settings: %s", settingsLoadResultName(lastSettingsLoadResult()));

  freeink::ui::drawText(target, badge.inset(freeink::ui::Insets{0, 18, 0, 0}), "!", bang);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(content.x + 132), static_cast<int16_t>(content.y + 46),
                                          static_cast<int16_t>(content.width - 164), 48},
                        "Settings Warning", title);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 162), content.width, 54},
                        settingsLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 224), content.width, 54},
                        sdLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 302), content.width, 84},
                        "Boot paused for 20 seconds so bad or missing companion settings are visible.",
                        body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 414), content.width, 68},
                        "Create the file on the SD card, then reboot when ready.",
                        body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 34), content.width, 24},
                        "/other/error", body);
  drawPageChrome(target);
  return tx;
}

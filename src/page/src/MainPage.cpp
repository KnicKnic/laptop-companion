#include "MainPage.h"

#include "AppState.h"
#include "PageDrawing.h"
#include "PageManager.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

MainPage::MainPage(EInkDisplay& display) : Page(display) {}

PageId MainPage::id() const {
  return PageId::Main;
}

const char* MainPage::name() const {
  return "main";
}

std::unique_ptr<RenderTransaction> MainPage::render(freeink::ui::DisplayTarget& target,
                                                    EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  const AppState state = copyAppState();

  const freeink::ui::Rect content = getPagePanel(target);
  
  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 3;

  char counterLine[96];
  snprintf(counterLine, sizeof(counterLine), "Power:%lu  Confirm:%lu  Back:%lu",
           static_cast<unsigned long>(state.powerInterruptCount),
           static_cast<unsigned long>(state.confirmPressCount), static_cast<unsigned long>(state.backPressCount));
  char secondaryCounterLine[96];
  snprintf(secondaryCounterLine, sizeof(secondaryCounterLine), "ADC L:%lu R:%lu U:%lu D:%lu",
           static_cast<unsigned long>(state.leftPressCount), static_cast<unsigned long>(state.rightPressCount),
           static_cast<unsigned long>(state.upPressCount), static_cast<unsigned long>(state.downPressCount));
  char totalLine[96];
  snprintf(totalLine, sizeof(totalLine), "Total button presses: %lu",
           static_cast<unsigned long>(state.totalButtonPressCount));
  char statusLine[96];
  snprintf(statusLine, sizeof(statusLine), "Runtime: %s   SD:%s   Settings:%s", tx->isX3Mode() ? "X3" : "X4",
           state.sdReady ? "ready" : "missing", state.companionSettingsValid ? "ready" : "warning");
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 20), content.width, 44},
                        "Hello, FreeInk!", title);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 112), content.width, 84},
                        "Root directory. Use the navigation row to enter companion, settings, or other.",
                        body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 236), content.width, 44},
                        counterLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 286), content.width, 44},
                        secondaryCounterLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 336), content.width, 44},
                        totalLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 72), content.width, 44},
                        statusLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 34), content.width, 24},
                        "Test page", body);
  drawPageChrome(target);
  return tx;
}

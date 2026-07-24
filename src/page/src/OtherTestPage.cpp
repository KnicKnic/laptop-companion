#include "OtherTestPage.h"

#include "AppState.h"
#include "PageDrawing.h"
#include "PageManager.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

OtherTestPage::OtherTestPage(EInkDisplay& display) : Page(display) {}

PageId OtherTestPage::id() const {
  return PageId::OtherTest;
}

const char* OtherTestPage::name() const {
  return "other-test";
}

std::unique_ptr<RenderTransaction> OtherTestPage::render(freeink::ui::DisplayTarget& target,
                                                         EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  const AppState state = copyAppState();

  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 2;

  char navigationLine[96];
  snprintf(navigationLine, sizeof(navigationLine), "Route: /other/test");
  char statusLine[96];
  snprintf(statusLine, sizeof(statusLine), "Runtime: %s   SD:%s   Settings:%s", tx->isX3Mode() ? "X3" : "X4",
           state.sdReady ? "ready" : "missing", state.companionSettingsValid ? "ready" : "warning");
  char buttonLine[96];
  snprintf(buttonLine, sizeof(buttonLine), "Buttons:%lu  Left:%lu  Right:%lu",
           static_cast<unsigned long>(state.totalButtonPressCount),
           static_cast<unsigned long>(state.leftPressCount), static_cast<unsigned long>(state.rightPressCount));

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 24), content.width, 44},
                        "Other Test", title);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 116), content.width, 44},
                        navigationLine, body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.y + 186), content.width, 44},
                        buttonLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 72), content.width, 44},
                        statusLine, body);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 34), content.width, 24},
                        "/other/test", body);
  drawPageChrome(target);
  return tx;
}

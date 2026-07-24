#include "SleepScreen.h"

#include "AppState.h"

#include <Arduino.h>

void drawStaticSleepImage(EInkDisplay& display, freeink::ui::DisplayTarget& target) {
  const AppState state = copyAppState();
  display.clearScreen(0xFF);
  target.setOrientation(freeink::ui::Orientation::Portrait);

  const freeink::ui::Rect screen{0, 0, target.logicalWidth(), target.logicalHeight()};
  const freeink::ui::Rect content = screen.inset(freeink::ui::Insets{32, 32, 32, 32});
  const freeink::ui::Rect badge{static_cast<int16_t>(content.x + 42), static_cast<int16_t>(content.y + 42), 96, 96};

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Center;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.maxLines = 3;

  char counterLine[96];
  snprintf(counterLine, sizeof(counterLine), "Final button count: %lu",
           static_cast<unsigned long>(state.totalButtonPressCount));

  target.fill(screen, freeink::ui::Paint::solid(freeink::ui::Color::White));
  target.fill(badge, freeink::ui::Paint::solid(freeink::ui::Color::Black), 6);
  target.fill(badge.inset(freeink::ui::Insets{14, 14, 14, 14}), freeink::ui::Paint::solid(freeink::ui::Color::White), 4);
  target.line(freeink::ui::Point{static_cast<int16_t>(badge.x + 48), static_cast<int16_t>(badge.y + 24)},
              freeink::ui::Point{static_cast<int16_t>(badge.x + 48), static_cast<int16_t>(badge.y + 54)}, 8,
              freeink::ui::Paint::solid(freeink::ui::Color::Black));
  target.stroke(freeink::ui::Rect{static_cast<int16_t>(badge.x + 30), static_cast<int16_t>(badge.y + 38), 36, 36},
                freeink::ui::Paint::solid(freeink::ui::Color::Black), 7, 18);

  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(content.x + 154), static_cast<int16_t>(content.y + 58),
                                          static_cast<int16_t>(content.width - 184), 48},
                        "POWER OFF", title);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(content.x + 154), static_cast<int16_t>(content.y + 116),
                                          static_cast<int16_t>(content.width - 184), 72},
                        "Press the power button again to wake.", body);
  freeink::ui::drawText(target, freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 96), content.width, 44},
                        counterLine, body);
}

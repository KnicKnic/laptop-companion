#include "PageDrawing.h"

#include <cstdio>

void formatDuration(char* out, size_t outLen, uint64_t us) {
  const uint64_t totalSeconds = us / 1000000ULL;
  const uint64_t hours = totalSeconds / 3600ULL;
  const uint64_t minutes = (totalSeconds / 60ULL) % 60ULL;
  const uint64_t seconds = totalSeconds % 60ULL;
  if (hours > 0) {
    snprintf(out, outLen, "%lluh %02llum %02llus", static_cast<unsigned long long>(hours),
             static_cast<unsigned long long>(minutes), static_cast<unsigned long long>(seconds));
  } else {
    snprintf(out, outLen, "%llum %02llus", static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
  }
}

uint32_t percentOf(uint64_t value, uint64_t total) {
  if (total == 0) return 0;
  return static_cast<uint32_t>((value * 100ULL) / total);
}

const char* boolText(bool value) {
  return value ? "on" : "off";
}

freeink::ui::Rect pageMainPanel(freeink::ui::DisplayTarget& target) {
  constexpr int16_t headerH = 22;
  constexpr int16_t navH = 0;
  constexpr int16_t footerH = 0;
  constexpr int16_t gap = 0;
  const int16_t y = static_cast<int16_t>(headerH + navH + gap);
  const int16_t h = static_cast<int16_t>(target.logicalHeight() - y - footerH - gap);
  return freeink::ui::Rect{0, y, target.logicalWidth(), h};
}

freeink::ui::Rect getPagePanel(freeink::ui::DisplayTarget& target){
  const int16_t width = target.logicalWidth();
  const int16_t height = target.logicalHeight();
  const freeink::ui::Rect screen{0, 0, width, height};
  const freeink::ui::Rect content = pageMainPanel(target).inset(freeink::ui::Insets{12, 8, 24, 18});

  target.fill(screen, freeink::ui::Paint::solid(freeink::ui::Color::White));
  return content;
}

void drawSettingRow(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& row, const char* label,
                    const char* value, bool selected) {
  freeink::ui::TextStyle text;
  text.align = freeink::ui::TextAlign::Left;
  text.maxLines = 1;
  text.color = selected ? freeink::ui::Color::White : freeink::ui::Color::Black;

  freeink::ui::TextStyle valueText;
  valueText.align = freeink::ui::TextAlign::Right;
  valueText.maxLines = 1;
  valueText.color = text.color;

  if (selected) {
    target.fill(row, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  }

  const char* marker = selected ? ">" : " ";
  char labelLine[64];
  snprintf(labelLine, sizeof(labelLine), "%s %s", marker, label);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(row.x + 8), static_cast<int16_t>(row.y + 8),
                                          static_cast<int16_t>(row.width - 150), static_cast<int16_t>(row.height - 8)},
                        labelLine, text);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(row.right() - 132), static_cast<int16_t>(row.y + 8),
                                          120, static_cast<int16_t>(row.height - 8)},
                        value, valueText);
}

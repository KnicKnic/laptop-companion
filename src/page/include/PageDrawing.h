#pragma once

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>

void formatDuration(char* out, size_t outLen, uint64_t us);
uint32_t percentOf(uint64_t value, uint64_t total);
const char* boolText(bool value);
freeink::ui::Rect pageMainPanel(freeink::ui::DisplayTarget& target);
freeink::ui::Rect getPagePanel(freeink::ui::DisplayTarget& target);
void drawSettingRow(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& row, const char* label,
                    const char* value, bool selected);

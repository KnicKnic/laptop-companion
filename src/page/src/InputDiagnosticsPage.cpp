#include "InputDiagnosticsPage.h"

#include "IsrInput.h"
#include "PageDrawing.h"
#include "PageManager.h"

#include <Arduino.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIFontSmall.h>

namespace {
constexpr freeink::ui::FontId kDiagnosticsFontSlot = 3;

freeink::ui::Rect diagnosticsContentRect(const freeink::ui::DisplayTarget& target) {
  // Keep this in sync with getPagePanel(), without using that helper here:
  // getPagePanel() clears the framebuffer and must only run during rendering.
  return freeink::ui::Rect{18, 34, static_cast<int16_t>(target.logicalWidth() - 26),
                           static_cast<int16_t>(target.logicalHeight() - 58)};
}

void formatUptime(char* buffer, size_t length, uint32_t milliseconds) {
  const uint32_t seconds = milliseconds / 1000;
  snprintf(buffer, length, "%lu:%02lu:%02lu", static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60), static_cast<unsigned long>(seconds % 60));
}

void formatAge(char* buffer, size_t length, uint32_t eventAtMs, uint32_t now) {
  if (eventAtMs == 0) {
    snprintf(buffer, length, "never");
    return;
  }
  const uint32_t seconds = (now - eventAtMs) / 1000;
  if (seconds < 60) {
    snprintf(buffer, length, "%lus ago", static_cast<unsigned long>(seconds));
  } else if (seconds < 3600) {
    snprintf(buffer, length, "%lum ago", static_cast<unsigned long>(seconds / 60));
  } else {
    snprintf(buffer, length, "%luh ago", static_cast<unsigned long>(seconds / 3600));
  }
}
}  // namespace

InputDiagnosticsPage::InputDiagnosticsPage(EInkDisplay& display) : Page(display) {}

PageId InputDiagnosticsPage::id() const {
  return PageId::InputDiagnostics;
}

const char* InputDiagnosticsPage::name() const {
  return "input-diagnostics";
}

bool InputDiagnosticsPage::handleButton(ButtonPressKind kind) {
  // E-ink is intentionally not updated for every observed event.  Confirm is
  // the explicit, low-frequency way to capture a new on-screen report.
  if (kind == ButtonPressKind::HomeKeyDown) {
    // Latch the edge here rather than relying on the live controller state at
    // render time: a bad pulse can have already released during the 1-second
    // e-ink update, but the video should still show that DOWN was received.
    homeDownRenderPending_ = true;
    return true;
  }
  if (kind == ButtonPressKind::Confirm) {
    homeDownRenderPending_ = false;
    return true;
  }
  return false;
}

freeink::ui::Rect InputDiagnosticsPage::refreshRect(freeink::ui::DisplayTarget& target) const {
  const freeink::ui::Rect content = diagnosticsContentRect(target);
  return freeink::ui::Rect{content.x, static_cast<int16_t>(content.bottom() - 104), content.width, 92};
}

bool InputDiagnosticsPage::handleTouch(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) {
  // This page is an evidence screen, not a dense control panel: any normal
  // touch in its content area requests a fresh snapshot. The large hit target
  // avoids precision tapping on an e-ink display.
  const freeink::ui::Rect content = diagnosticsContentRect(target);
  return x >= content.x && x < content.right() && y >= content.y && y < content.bottom();
}

std::unique_ptr<RenderTransaction> InputDiagnosticsPage::render(freeink::ui::DisplayTarget& target,
                                                                EInkDisplay::RefreshMode mode, bool forceDraw) {
  (void)forceDraw;
  auto tx = beginRender(mode);
  target.setFont(kDiagnosticsFontSlot, freeink::ui::kNotoSansSmallFont);
  const InputDiagnosticsSnapshot diagnostics = copyInputDiagnostics();
  const uint32_t now = millis();
  const freeink::ui::Rect content = getPagePanel(target);

  freeink::ui::TextStyle title;
  title.font = freeink::ui::FONT_SLOT_BODY;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;

  freeink::ui::TextStyle body = title;
  body.font = kDiagnosticsFontSlot;
  body.maxLines = 1;

  char capturedAt[24];
  formatUptime(capturedAt, sizeof(capturedAt), now);
  char lastHomeAge[24];
  formatAge(lastHomeAge, sizeof(lastHomeAge), diagnostics.lastHomeKeyReleaseAtMs, now);
  char lastDeliveredAge[24];
  formatAge(lastDeliveredAge, sizeof(lastDeliveredAge), diagnostics.lastDirectoryEventAtMs, now);
  const uint32_t lastTransitionAt = diagnostics.lastHomeTransitionWasDown ? diagnostics.lastHomeKeyDownAtMs
                                                                            : diagnostics.lastHomeKeyReleaseAtMs;
  char lastTransitionAtText[24];
  formatUptime(lastTransitionAtText, sizeof(lastTransitionAtText), lastTransitionAt);

  char line[160];
  int16_t y = static_cast<int16_t>(content.y + 8);
  auto drawLine = [&](const char* text) {
    freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 22}, text, body);
    y = static_cast<int16_t>(y + 25);
  };

  freeink::ui::drawText(target, freeink::ui::Rect{content.x, y, content.width, 32}, "Input / Home-key Diagnostic", title);
  y = static_cast<int16_t>(y + 37);
  drawLine(homeDownRenderPending_ ? "CAPTURED EVENT: HOME KEY DOWN" : "Captured event: manual screen refresh");
  homeDownRenderPending_ = false;
  snprintf(line, sizeof(line), "Screen snapshot: uptime %s (manual refresh only)", capturedAt);
  drawLine(line);
  snprintf(line, sizeof(line), "LAST HOME EDGE: %s at %s", diagnostics.lastHomeTransitionWasDown ? "DOWN" : "UP",
           lastTransitionAt == 0 ? "--" : lastTransitionAtText);
  drawLine(line);
  if (diagnostics.lastHomeKeyDownAtMs == 0) {
    snprintf(line, sizeof(line), "Last DOWN: no Home-key edge observed yet");
  } else {
    snprintf(line, sizeof(line), "Last DOWN: GT911 0x%02X, contacts %u%s", diagnostics.lastHomeKeyDownStatus,
             diagnostics.lastHomeKeyDownContactCount,
             diagnostics.lastHomeKeyDownContactCount == 0 ? " (no screen tap)" : "");
  }
  drawLine(line);
  if (diagnostics.lastHomeKeyReleaseAtMs == 0) {
    snprintf(line, sizeof(line), "Last UP: no Home-key edge observed yet");
  } else {
    snprintf(line, sizeof(line), "Last UP:   GT911 0x%02X, contacts %u%s", diagnostics.lastHomeKeyReleaseStatus,
             diagnostics.lastHomeKeyReleaseContactCount,
             diagnostics.lastHomeKeyReleaseContactCount == 0 ? " (no screen tap)" : "");
  }
  drawLine(line);
  drawLine("Recording continues while this e-ink screen stays unchanged.");
  snprintf(line, sizeof(line), "GT911: %s addr 0x%02X  Home key now: %s  status:0x%02X",
           diagnostics.gt911Detected ? "detected" : "NOT detected", diagnostics.gt911Address,
           diagnostics.homeKeyDown ? "DOWN" : "up", diagnostics.lastGt911Status);
  drawLine(line);
  if (diagnostics.gt911KeyConfigRead) {
    snprintf(line, sizeof(line), "Loaded key cfg v0x%02X: touch %u, release %u", diagnostics.gt911ConfigVersion,
             diagnostics.gt911KeyTouchLevel, diagnostics.gt911KeyLeaveLevel);
    drawLine(line);
    snprintf(line, sizeof(line), "Key sens: 0x%02X / 0x%02X   restraint: 0x%02X / 0x%02X",
             diagnostics.gt911KeySensitivity12, diagnostics.gt911KeySensitivity34, diagnostics.gt911KeyRestrain,
             diagnostics.gt911KeyRestrainTime);
    drawLine(line);
  } else {
    drawLine("Loaded key configuration: unavailable");
  }
  snprintf(line, sizeof(line), "Home down transitions: %lu", static_cast<unsigned long>(diagnostics.homeKeyDownTransitions));
  drawLine(line);
  snprintf(line, sizeof(line), "Home releases observed: %lu  last: %s",
           static_cast<unsigned long>(diagnostics.homeKeyReleaseEvents), lastHomeAge);
  drawLine(line);
  snprintf(line, sizeof(line), "Directory actions delivered: %lu  last: %s",
           static_cast<unsigned long>(diagnostics.directoryEvents), lastDeliveredAge);
  drawLine(line);
  snprintf(line, sizeof(line), "GT911 status reads: %lu  failed: %lu  point failed: %lu",
           static_cast<unsigned long>(diagnostics.gt911StatusReads),
           static_cast<unsigned long>(diagnostics.gt911StatusReadFailures),
           static_cast<unsigned long>(diagnostics.gt911PointReadFailures));
  drawLine(line);
  snprintf(line, sizeof(line), "GPIO interrupts: touch %lu  buttons %lu  UI events %lu",
           static_cast<unsigned long>(diagnostics.touchInterrupts),
           static_cast<unsigned long>(diagnostics.buttonInterrupts),
           static_cast<unsigned long>(diagnostics.deliveredEvents));
  drawLine(line);
  drawLine("Recent delivered events (newest first):");
  for (uint8_t i = 0; i < diagnostics.recentEventCount && i < 5; ++i) {
    char eventAt[24];
    formatUptime(eventAt, sizeof(eventAt), diagnostics.recentEvents[i].uptimeMs);
    snprintf(line, sizeof(line), "%s  %s", eventAt, inputPressKindName(diagnostics.recentEvents[i].kind));
    drawLine(line);
  }

  const freeink::ui::Rect refresh = refreshRect(target);
  target.fill(refresh, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  freeink::ui::TextStyle footer = body;
  footer.align = freeink::ui::TextAlign::Center;
  footer.maxLines = 2;
  footer.color = freeink::ui::Color::White;
  freeink::ui::drawText(target, refresh.inset(freeink::ui::Insets{10, 12, 10, 0}),
                        "TAP ANYWHERE ON THIS PAGE\nTO REFRESH SNAPSHOT", footer);
  drawPageChrome(target);
  return tx;
}

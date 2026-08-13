#include "PageManager.h"

#include "AppLog.h"
#include "AppState.h"
#include "CompanionSettingsWarningPage.h"
#include "CompanionPage.h"
#include "CompanionStatsPage.h"
#include "InputDiagnosticsPage.h"
#include "MainPage.h"
#include "OtherTestPage.h"
#include "Page.h"
#include "PowerStatsPage.h"
#include "SettingsPage.h"
#include "companion/CompanionBleService.h"

#include <BatteryMonitor.h>
#include <FreeInkUI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>

namespace {

struct Route {
  PageId id;
  PageId parent;
  const char* path;
  const char* label;
  bool companionSubtree;
};

MainPage* mainPage = nullptr;
CompanionPage* companionPage = nullptr;
CompanionStatsPage* companionStatsPage = nullptr;
SettingsPage* settingsPage = nullptr;
OtherTestPage* otherTestPage = nullptr;
PowerStatsPage* powerStatsPage = nullptr;
CompanionSettingsWarningPage* companionSettingsWarningPage = nullptr;
InputDiagnosticsPage* inputDiagnosticsPage = nullptr;
EInkDisplay* pageDisplay = nullptr;
freeink::ui::DisplayTarget* pageTarget = nullptr;

Page* pages[] = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

constexpr Route routes[] = {
    {PageId::Main, PageId::Main, "/", "/", false},
    {PageId::Companion, PageId::Main, "/companion", "companion", true},
    {PageId::CompanionStats, PageId::Companion, "/companion/stats", "stats", true},
    {PageId::Settings, PageId::Main, "/settings", "settings", false},
    {PageId::OtherTest, PageId::Main, "/other/test", "test", false},
    {PageId::PowerStats, PageId::Main, "/other/power-stats", "power-stats", false},
    {PageId::CompanionSettingsWarning, PageId::Main, "/other/error", "error", false},
    {PageId::InputDiagnostics, PageId::Main, "/diagnostics/input", "input diagnostics", false},
};

constexpr PageId kOtherTestRoute = PageId::OtherTest;
constexpr PageId kOtherPowerStatsRoute = PageId::PowerStats;
constexpr PageId kOtherErrorRoute = PageId::CompanionSettingsWarning;
constexpr PageId kInputDiagnosticsRoute = PageId::InputDiagnostics;

SemaphoreHandle_t pageMutex = nullptr;
PageId currentPage = PageId::Companion;
uint8_t rootNavIndex = 0;
uint8_t companionNavIndex = 0;
uint8_t otherNavIndex = 0;
bool directoryOpen = false;
bool directoryOpenedByHomeKey = false;
uint8_t directoryIndex = 1;

bool batteryCached = false;
bool batteryKnown = false;
uint16_t batteryPercent = 0;
unsigned long batteryReadAtMs = 0;
constexpr unsigned long kBatteryRefreshMs = 5UL * 60UL * 1000UL;
// The CW2017 can briefly return 0% while its profile settles after boot. Don't
// pin that provisional value in the page chrome's five-minute cache.
constexpr unsigned long kBatteryRetryMs = 2UL * 1000UL;

constexpr size_t pageCount() {
  return sizeof(pages) / sizeof(pages[0]);
}

constexpr size_t routeCount() {
  return sizeof(routes) / sizeof(routes[0]);
}

const Route& routeFor(PageId page) {
  for (const Route& route : routes) {
    if (route.id == page) return route;
  }
  return routes[0];
}

int routeIndex(PageId page) {
  for (size_t i = 0; i < routeCount(); ++i) {
    if (routes[i].id == page) return static_cast<int>(i);
  }
  return 0;
}

int pageIndex(PageId page) {
  for (size_t i = 0; i < pageCount(); ++i) {
    if (pages[i] != nullptr && pages[i]->id() == page) return static_cast<int>(i);
  }
  return 0;
}

Page& pageFor(PageId page) {
  return *pages[pageIndex(page)];
}

bool isCompanionRoute(PageId page) {
  return routeFor(page).companionSubtree;
}

void refreshBatteryForRender() {
  const unsigned long now = millis();
  const unsigned long refreshInterval = (batteryKnown && batteryPercent > 0) ? kBatteryRefreshMs : kBatteryRetryMs;
  if (batteryCached && now - batteryReadAtMs < refreshInterval) return;

  BatteryMonitor battery;
  const BatteryMonitor::Status status = battery.readStatus();
  batteryKnown = status.percentageKnown;
  batteryPercent = status.percentage > 100 ? 100 : status.percentage;
  batteryReadAtMs = now;
  batteryCached = true;
  setBootBatteryPercentage(batteryKnown, batteryPercent);
}

PageId rootNavPage(uint8_t index) {
  switch (index % 3) {
    case 0:
      return PageId::Companion;
    case 1:
      return PageId::Settings;
    default:
      return kOtherTestRoute;
  }
}

const char* rootNavLabel(uint8_t index) {
  switch (index % 3) {
    case 0:
      return "companion";
    case 1:
      return "settings";
    default:
      return "other";
  }
}

PageId companionNavPage(uint8_t) {
  return PageId::CompanionStats;
}

const char* companionNavLabel(uint8_t) {
  return "stats";
}

PageId otherNavPage(uint8_t index) {
  switch (index % 4) {
    case 0:
      return kOtherTestRoute;
    case 1:
      return kOtherPowerStatsRoute;
    case 2:
      return kOtherErrorRoute;
    default:
      return kInputDiagnosticsRoute;
  }
}

const char* otherNavLabel(uint8_t index) {
  switch (index % 4) {
    case 0:
      return "test";
    case 1:
      return "power-stats";
    case 2:
      return "error";
    default:
      return "input diag";
  }
}

bool routeInOther(PageId page) {
  return page == kOtherTestRoute || page == kOtherPowerStatsRoute || page == kOtherErrorRoute ||
         page == kInputDiagnosticsRoute;
}

uint8_t navItemCountFor(PageId page) {
  if (page == PageId::Main) return 3;
  if (page == PageId::Companion) return 1;
  if (page == PageId::CompanionStats) return 1;
  if (routeInOther(page)) return 4;
  return 3;
}

uint8_t& navIndexFor(PageId page) {
  if (page == PageId::Companion || page == PageId::CompanionStats) return companionNavIndex;
  if (routeInOther(page)) return otherNavIndex;
  return rootNavIndex;
}

PageId selectedNavPageFor(PageId page) {
  if (page == PageId::Companion || page == PageId::CompanionStats) return companionNavPage(companionNavIndex);
  if (routeInOther(page)) return otherNavPage(otherNavIndex);
  return rootNavPage(rootNavIndex);
}

const char* navLabelFor(PageId page, uint8_t index) {
  if (page == PageId::Companion || page == PageId::CompanionStats) return companionNavLabel(index);
  if (routeInOther(page)) return otherNavLabel(index);
  return rootNavLabel(index);
}

PageId parentFor(PageId page) {
  return routeFor(page).parent;
}

void syncNavSelection(PageId page) {
  if (page == PageId::Companion || page == PageId::CompanionStats) {
    rootNavIndex = 0;
    companionNavIndex = 0;
  } else if (page == PageId::Settings) {
    rootNavIndex = 1;
  } else if (routeInOther(page)) {
    rootNavIndex = 2;
    if (page == kOtherTestRoute) otherNavIndex = 0;
    else if (page == kOtherPowerStatsRoute) otherNavIndex = 1;
    else if (page == kOtherErrorRoute) otherNavIndex = 2;
    else otherNavIndex = 3;
  }
}

void syncDirectorySelection(PageId page) {
  directoryIndex = static_cast<uint8_t>(routeIndex(page));
}

PageId setCurrentPage(PageId page) {
  Page* leaving = nullptr;
  Page* entering = &pageFor(page);
  PageId previous = PageId::Main;

  xSemaphoreTake(pageMutex, portMAX_DELAY);
  previous = currentPage;
  if (currentPage != entering->id()) {
    leaving = &pageFor(currentPage);
  }
  currentPage = entering->id();
  syncNavSelection(currentPage);
  syncDirectorySelection(currentPage);
  const PageId selected = currentPage;
  xSemaphoreGive(pageMutex);

  if (leaving) {
    leaving->onLeave();
    if (isCompanionRoute(previous) && !isCompanionRoute(selected)) {
      CompanionBleService::getInstance().end();
    }
    entering->onEnter();
  }
  return selected;
}

void moveDirectorySelection(int direction) {
  const uint8_t count = static_cast<uint8_t>(routeCount());
  directoryIndex = static_cast<uint8_t>((directoryIndex + count + direction) % count);
}

void moveNavSelection(int direction) {
  uint8_t& index = navIndexFor(currentPage);
  const uint8_t count = navItemCountFor(currentPage);
  index = static_cast<uint8_t>((index + count + direction) % count);
}

void drawNavItem(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& rect, const char* label,
                 bool selected) {
  if (selected) {
    target.fill(rect, freeink::ui::Paint::solid(freeink::ui::Color::Black), 4);
  } else {
    target.stroke(rect, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 4);
  }

  freeink::ui::TextStyle style;
  style.align = freeink::ui::TextAlign::Center;
  style.maxLines = 1;
  style.color = selected ? freeink::ui::Color::White : freeink::ui::Color::Black;
  freeink::ui::drawText(target, rect.inset(freeink::ui::Insets{4, 6, 4, 0}), label, style);
}

void drawDirectoryRow(freeink::ui::DisplayTarget& target, const freeink::ui::Rect& row, const char* path,
                      bool selected) {
  target.fill(row, freeink::ui::Paint::solid(freeink::ui::Color::White));
  if (selected) {
    target.fill(row, freeink::ui::Paint::solid(freeink::ui::Color::Black), 2);
  }

  freeink::ui::TextStyle style;
  style.align = freeink::ui::TextAlign::Left;
  style.maxLines = 1;
  style.color = selected ? freeink::ui::Color::White : freeink::ui::Color::Black;

  freeink::ui::drawText(target, row.inset(freeink::ui::Insets{12, 8, 12, 0}), path, style);
}

freeink::ui::Rect directoryPopupRect(freeink::ui::DisplayTarget& target) {
  const int16_t width = target.logicalWidth();
  const int16_t height = target.logicalHeight();
  constexpr int16_t kHeaderHeight = 78;
  constexpr int16_t kRowHeightWithGap = 46;
  constexpr int16_t kFooterHeight = 42;
  const int16_t popupHeight = static_cast<int16_t>(kHeaderHeight + routeCount() * kRowHeightWithGap + kFooterHeight);
  return freeink::ui::Rect{24, static_cast<int16_t>((height - popupHeight) / 2), static_cast<int16_t>(width - 48),
                           popupHeight};
}

freeink::ui::Rect directoryRowRect(freeink::ui::DisplayTarget& target, uint8_t index) {
  constexpr int16_t rowH = 42;
  constexpr int16_t rowGap = 4;
  const freeink::ui::Rect popup = directoryPopupRect(target);
  return freeink::ui::Rect{static_cast<int16_t>(popup.x + 16),
                           static_cast<int16_t>(popup.y + 78 + index * (rowH + rowGap)),
                           static_cast<int16_t>(popup.width - 32), rowH};
}

int directoryHitIndex(freeink::ui::DisplayTarget& target, int16_t x, int16_t y) {
  const freeink::ui::Rect first = directoryRowRect(target, 0);
  const freeink::ui::Rect last = directoryRowRect(target, static_cast<uint8_t>(routeCount() - 1));
  if (x < first.x || x >= first.right()) return -1;

  // The visible gutters remain, but they are not dead touch zones.  Between
  // rows, choose the nearest target; the outer boundary stays inside the list.
  const int16_t top = static_cast<int16_t>(first.y - 2);
  const int16_t bottom = static_cast<int16_t>(last.bottom() + 2);
  if (y < top || y >= bottom) return -1;

  uint8_t closest = 0;
  int closestDistance = 0x7FFF;
  for (uint8_t i = 0; i < routeCount(); ++i) {
    const freeink::ui::Rect row = directoryRowRect(target, i);
    const int distance = y > row.y + row.height / 2 ? y - (row.y + row.height / 2)
                                                      : (row.y + row.height / 2) - y;
    if (distance < closestDistance) {
      closestDistance = distance;
      closest = i;
    }
  }
  return closest;
}

bool panelWindowForLogicalRect(const freeink::ui::DisplayTarget& target, freeink::ui::Rect logical, uint16_t& x,
                               uint16_t& y, uint16_t& w, uint16_t& h) {
  if (pageDisplay == nullptr || logical.empty()) return false;

  int16_t px = 0;
  int16_t py = 0;
  int16_t pw = 0;
  int16_t ph = 0;
  switch (target.orientation()) {
    case freeink::ui::Orientation::Portrait:
      px = logical.y;
      py = static_cast<int16_t>(pageDisplay->getDisplayHeight() - logical.right());
      pw = logical.height;
      ph = logical.width;
      break;
    case freeink::ui::Orientation::PortraitInverted:
      px = static_cast<int16_t>(pageDisplay->getDisplayWidth() - logical.bottom());
      py = logical.x;
      pw = logical.height;
      ph = logical.width;
      break;
    case freeink::ui::Orientation::LandscapeClockwise:
      px = static_cast<int16_t>(pageDisplay->getDisplayWidth() - logical.right());
      py = static_cast<int16_t>(pageDisplay->getDisplayHeight() - logical.bottom());
      pw = logical.width;
      ph = logical.height;
      break;
    case freeink::ui::Orientation::LandscapeCounterClockwise:
    default:
      px = logical.x;
      py = logical.y;
      pw = logical.width;
      ph = logical.height;
      break;
  }

  if (px < 0) {
    pw = static_cast<int16_t>(pw + px);
    px = 0;
  }
  if (py < 0) {
    ph = static_cast<int16_t>(ph + py);
    py = 0;
  }
  if (px >= pageDisplay->getDisplayWidth() || py >= pageDisplay->getDisplayHeight() || pw <= 0 || ph <= 0) {
    return false;
  }
  if (px + pw > pageDisplay->getDisplayWidth()) pw = static_cast<int16_t>(pageDisplay->getDisplayWidth() - px);
  if (py + ph > pageDisplay->getDisplayHeight()) ph = static_cast<int16_t>(pageDisplay->getDisplayHeight() - py);

  const int16_t alignedX = static_cast<int16_t>(px & ~0x7);
  const int16_t alignedRight = static_cast<int16_t>(((px + pw + 7) / 8) * 8);
  x = static_cast<uint16_t>(alignedX);
  y = static_cast<uint16_t>(py);
  w = static_cast<uint16_t>(alignedRight - alignedX);
  h = static_cast<uint16_t>(ph);
  if (x + w > pageDisplay->getDisplayWidth()) w = static_cast<uint16_t>(pageDisplay->getDisplayWidth() - x);
  return w > 0 && h > 0;
}

void drawDirectoryOverlay(freeink::ui::DisplayTarget& target) {
  if (!directoryOpen) return;

  const freeink::ui::Rect popup = directoryPopupRect(target);
  target.fill(popup, freeink::ui::Paint::solid(freeink::ui::Color::White));
  target.stroke(popup, freeink::ui::Paint::solid(freeink::ui::Color::Black), 2, 4);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;
  freeink::ui::drawText(target, popup.inset(freeink::ui::Insets{16, 12, 16, 0}),
                        directoryOpenedByHomeKey ? "Directory (Home key released)" : "Directory", title);

  freeink::ui::TextStyle body = title;
  body.maxLines = 1;
  char currentLine[96];
  snprintf(currentLine, sizeof(currentLine), "Current: %s", routeFor(currentPage).path);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(popup.x + 16), static_cast<int16_t>(popup.y + 42),
                                          static_cast<int16_t>(popup.width - 32), 26},
                        currentLine, body);

  for (uint8_t i = 0; i < routeCount(); ++i) {
    drawDirectoryRow(target, directoryRowRect(target, i), routes[i].path, i == directoryIndex);
  }

  freeink::ui::TextStyle footer;
  footer.align = freeink::ui::TextAlign::Center;
  footer.maxLines = 1;
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(popup.x + 16),
                                          static_cast<int16_t>(popup.bottom() - 32),
                                          static_cast<int16_t>(popup.width - 32), 24},
                        "Tap a route to open it. Home closes.", footer);
}

}  // namespace

bool beginPageManager(EInkDisplay& display, PageId initialPage) {
  pageDisplay = &display;
  if (pageMutex == nullptr) {
    pageMutex = xSemaphoreCreateMutex();
  }
  if (pageMutex == nullptr) return false;

  if (pageTarget == nullptr) {
    pageTarget = new (std::nothrow) freeink::ui::DisplayTarget(
        display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(), display.getDisplayWidthBytes(),
        freeink::ui::Orientation::Portrait);
  }
  if (pageTarget == nullptr) return false;

  if (mainPage == nullptr) {
    mainPage = new MainPage(display);
    companionPage = new CompanionPage(display);
    companionStatsPage = new CompanionStatsPage(display);
    settingsPage = new SettingsPage(display);
    otherTestPage = new OtherTestPage(display);
    powerStatsPage = new PowerStatsPage(display);
    companionSettingsWarningPage = new CompanionSettingsWarningPage(display);
    inputDiagnosticsPage = new InputDiagnosticsPage(display);

    pages[0] = mainPage;
    pages[1] = companionPage;
    pages[2] = companionStatsPage;
    pages[3] = settingsPage;
    pages[4] = otherTestPage;
    pages[5] = powerStatsPage;
    pages[6] = companionSettingsWarningPage;
    pages[7] = inputDiagnosticsPage;
  }

  currentPage = pageFor(initialPage).id();
  syncNavSelection(currentPage);
  syncDirectorySelection(currentPage);
  pageFor(currentPage).onEnter();
  return true;
}

freeink::ui::DisplayTarget* pageDisplayTarget() {
  return pageTarget;
}

PageId activePage() {
  xSemaphoreTake(pageMutex, portMAX_DELAY);
  const PageId page = currentPage;
  xSemaphoreGive(pageMutex);
  return page;
}

PageId showPage(PageId page) {
  return setCurrentPage(page);
}

PageId showNextPage() {
  moveNavSelection(1);
  return setCurrentPage(selectedNavPageFor(currentPage));
}

PageId showPreviousPage() {
  return setCurrentPage(parentFor(currentPage));
}

PageButtonResult handlePageButton(ButtonPressKind kind) {
  const PageId page = activePage();

  xSemaphoreTake(pageMutex, portMAX_DELAY);
  const bool overlayOpen = directoryOpen;
  xSemaphoreGive(pageMutex);

  if (overlayOpen) {
    switch (kind) {
      case ButtonPressKind::Back:
      case ButtonPressKind::Directory:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        directoryOpen = false;
        directoryOpenedByHomeKey = false;
        xSemaphoreGive(pageMutex);
        return PageButtonResult{activePage(), true, false, true};
      case ButtonPressKind::Left:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        moveDirectorySelection(-1);
        xSemaphoreGive(pageMutex);
        return PageButtonResult{activePage(), true, true};
      case ButtonPressKind::Right:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        moveDirectorySelection(1);
        xSemaphoreGive(pageMutex);
        return PageButtonResult{activePage(), true, true};
      case ButtonPressKind::Confirm: {
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        const PageId selected = routes[directoryIndex].id;
        directoryOpen = false;
        directoryOpenedByHomeKey = false;
        xSemaphoreGive(pageMutex);
        logPrintf("Directory selected: %s\n", routeFor(selected).path);
        return PageButtonResult{setCurrentPage(selected), true, false};
      }
      case ButtonPressKind::Up:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        moveDirectorySelection(-1);
        xSemaphoreGive(pageMutex);
        return PageButtonResult{activePage(), true, true};
      case ButtonPressKind::Down:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        moveDirectorySelection(1);
        xSemaphoreGive(pageMutex);
        return PageButtonResult{activePage(), true, true};
      case ButtonPressKind::Gpio1:
      case ButtonPressKind::Gpio2:
      case ButtonPressKind::Power:
      case ButtonPressKind::Touch:
      case ButtonPressKind::HomeKeyDown:
        return PageButtonResult{activePage(), false, false};
    }
  }

  switch (kind) {
    case ButtonPressKind::Back:
    case ButtonPressKind::Directory:
      xSemaphoreTake(pageMutex, portMAX_DELAY);
      syncDirectorySelection(page);
      directoryOpen = true;
      directoryOpenedByHomeKey = kind == ButtonPressKind::Directory;
      xSemaphoreGive(pageMutex);
      return PageButtonResult{page, true, true};
    case ButtonPressKind::Up:
    case ButtonPressKind::Down:
      if (pageFor(page).handleButton(kind)) {
        return PageButtonResult{activePage(), true, false};
      }
      return PageButtonResult{
          kind == ButtonPressKind::Up ? showPreviousPage() : showNextPage(), true, false};
    case ButtonPressKind::Left:
    case ButtonPressKind::Right:
    case ButtonPressKind::Confirm:
      return PageButtonResult{activePage(), pageFor(page).handleButton(kind), false};
    case ButtonPressKind::Gpio1:
    case ButtonPressKind::Gpio2:
    case ButtonPressKind::Power:
    case ButtonPressKind::Touch:
      return PageButtonResult{activePage(), false, false};
    case ButtonPressKind::HomeKeyDown:
      return PageButtonResult{activePage(), pageFor(page).handleButton(kind), false};
  }
  return PageButtonResult{activePage(), false, false};
}

PageButtonResult handlePageTouch(float panelX, float panelY) {
  if (pageTarget == nullptr) return PageButtonResult{activePage(), false, false};

  // InputManager reports normalized panel-native coordinates.  Use the same
  // orientation mapping as FreeInkUI's snapshotFrom(InputManager, device)
  // adapter, rather than maintaining a page-local inverse transform.
  const freeink::ui::Point touch = freeink::ui::touchToLogical(pageTarget->deviceContext(), panelX, panelY);
  const int16_t x = touch.x;
  const int16_t y = touch.y;

  xSemaphoreTake(pageMutex, portMAX_DELAY);
  const bool overlayOpen = directoryOpen;
  if (overlayOpen) {
    const int hit = directoryHitIndex(*pageTarget, x, y);
    if (hit >= 0) {
      const PageId selected = routes[hit].id;
      directoryOpen = false;
      xSemaphoreGive(pageMutex);
      return PageButtonResult{setCurrentPage(selected), true, false};
    }
    directoryOpen = false;
    xSemaphoreGive(pageMutex);
    return PageButtonResult{activePage(), true, false, true};
  }
  xSemaphoreGive(pageMutex);

  const PageId page = activePage();
  return PageButtonResult{page, pageFor(page).handleTouch(*pageTarget, x, y), false};
}

const char* pageName(PageId page) {
  return routeFor(page).path;
}

void drawPageChrome(freeink::ui::DisplayTarget& target) {
  const int16_t width = target.logicalWidth();

  char battery[20];
  if (batteryCached && batteryKnown) {
    snprintf(battery, sizeof(battery), "%u%%", batteryPercent);
  } else {
    snprintf(battery, sizeof(battery), "--%%");
  }

  freeink::ui::InputSnapshot input;
  freeink::ui::InteractionBuffer<1> interactions;
  freeink::ui::Frame<1> frame(target, target.deviceContext(), input, interactions);
  freeink::ui::StatusBarProps status;
  status.leading = "Nicholas.Maliwack@gmail.com";
  status.trailing = battery;
  status.fillBackground = true;
  status.text.maxLines = 1;
  status.trailingSecondary = nullptr;
  freeink::ui::statusBar(frame, freeink::ui::Rect{0, 0, width, 38}, status);

  freeink::ui::BatteryIndicatorProps batteryProps;
  batteryProps.percent = batteryKnown ? static_cast<uint8_t>(batteryPercent) : 0;
  batteryProps.label = battery;
  batteryProps.text.align = freeink::ui::TextAlign::Right;
  batteryProps.text.maxLines = 1;
  batteryProps.glyphWidth = 28;
  batteryProps.glyphHeight = 14;
  batteryProps.gap = 6;
  freeink::ui::batteryIndicator(frame, freeink::ui::Rect{static_cast<int16_t>(width - 122), 3, 112, 30},
                                batteryProps);

}

void renderActivePage(EInkDisplay::RefreshMode mode) {
  if (pageDisplay == nullptr || pageTarget == nullptr) return;

  refreshBatteryForRender();
  xSemaphoreTake(pageMutex, portMAX_DELAY);
  Page& page = pageFor(currentPage);
  constexpr bool forceDraw = true;
  pageTarget->setOrientation(freeink::ui::Orientation::Portrait);
  page.preRender(*pageTarget, mode, forceDraw);
  std::unique_ptr<RenderTransaction> transaction = page.render(*pageTarget, mode, forceDraw);
  if (transaction) {
    drawDirectoryOverlay(*pageTarget);
  }
  page.postRender(*pageTarget, mode, forceDraw);
  xSemaphoreGive(pageMutex);
  transaction.reset();
}

void renderDirectoryOverlay(EInkDisplay::RefreshMode mode) {
  if (pageDisplay == nullptr || pageTarget == nullptr) return;

  freeink::ui::Rect dirty;
  bool shouldDisplay = false;
  xSemaphoreTake(pageMutex, portMAX_DELAY);
  pageTarget->setOrientation(freeink::ui::Orientation::Portrait);
  if (directoryOpen) {
    drawDirectoryOverlay(*pageTarget);
    dirty = directoryPopupRect(*pageTarget);
    shouldDisplay = true;
  }
  xSemaphoreGive(pageMutex);

  if (!shouldDisplay) return;

  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  if (panelWindowForLogicalRect(*pageTarget, dirty, x, y, w, h)) {
    pageDisplay->displayWindow(x, y, w, h);
  } else {
    pageDisplay->displayBuffer(mode);
  }
}

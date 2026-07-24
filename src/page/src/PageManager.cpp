#include "PageManager.h"

#include "AppLog.h"
#include "AppState.h"
#include "CompanionSettingsWarningPage.h"
#include "CompanionPage.h"
#include "CompanionStatsPage.h"
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
};

constexpr Route routes[] = {
    {PageId::Main, PageId::Main, "/", "/", false},
    {PageId::Companion, PageId::Main, "/companion", "companion", true},
    {PageId::CompanionStats, PageId::Companion, "/companion/stats", "stats", true},
    {PageId::Settings, PageId::Main, "/settings", "settings", false},
    {PageId::OtherTest, PageId::Main, "/other/test", "test", false},
    {PageId::PowerStats, PageId::Main, "/other/power-stats", "power-stats", false},
    {PageId::CompanionSettingsWarning, PageId::Main, "/other/error", "error", false},
};

constexpr PageId kOtherTestRoute = PageId::OtherTest;
constexpr PageId kOtherPowerStatsRoute = PageId::PowerStats;
constexpr PageId kOtherErrorRoute = PageId::CompanionSettingsWarning;

SemaphoreHandle_t pageMutex = nullptr;
PageId currentPage = PageId::Companion;
uint8_t rootNavIndex = 0;
uint8_t companionNavIndex = 0;
uint8_t otherNavIndex = 0;
bool directoryOpen = false;
uint8_t directoryIndex = 1;

bool batteryCached = false;
bool batteryKnown = false;
uint16_t batteryPercent = 0;
unsigned long batteryReadAtMs = 0;
constexpr unsigned long kBatteryRefreshMs = 5UL * 60UL * 1000UL;

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
  if (batteryCached && now - batteryReadAtMs < kBatteryRefreshMs) return;

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
  switch (index % 3) {
    case 0:
      return kOtherTestRoute;
    case 1:
      return kOtherPowerStatsRoute;
    default:
      return kOtherErrorRoute;
  }
}

const char* otherNavLabel(uint8_t index) {
  switch (index % 3) {
    case 0:
      return "test";
    case 1:
      return "power-stats";
    default:
      return "error";
  }
}

bool routeInOther(PageId page) {
  return page == kOtherTestRoute || page == kOtherPowerStatsRoute || page == kOtherErrorRoute;
}

uint8_t navItemCountFor(PageId page) {
  if (page == PageId::Main) return 3;
  if (page == PageId::Companion) return 1;
  if (page == PageId::CompanionStats) return 1;
  if (routeInOther(page)) return 3;
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
    else otherNavIndex = 2;
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

  freeink::ui::drawText(target, row.inset(freeink::ui::Insets{8, 3, 8, 0}), path, style);
}

void drawDirectoryOverlay(freeink::ui::DisplayTarget& target) {
  if (!directoryOpen) return;

  const int16_t width = target.logicalWidth();
  const int16_t height = target.logicalHeight();
  const freeink::ui::Rect popup{24, static_cast<int16_t>((height - 430) / 2), static_cast<int16_t>(width - 48), 430};
  target.fill(popup, freeink::ui::Paint::solid(freeink::ui::Color::White));
  target.stroke(popup, freeink::ui::Paint::solid(freeink::ui::Color::Black), 2, 4);

  freeink::ui::TextStyle title;
  title.align = freeink::ui::TextAlign::Left;
  title.maxLines = 1;
  freeink::ui::drawText(target, popup.inset(freeink::ui::Insets{16, 12, 16, 0}), "Directory", title);

  freeink::ui::TextStyle body = title;
  body.maxLines = 1;
  char currentLine[96];
  snprintf(currentLine, sizeof(currentLine), "Current: %s", routeFor(currentPage).path);
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(popup.x + 16), static_cast<int16_t>(popup.y + 42),
                                          static_cast<int16_t>(popup.width - 32), 26},
                        currentLine, body);

  constexpr int16_t rowH = 34;
  int16_t y = static_cast<int16_t>(popup.y + 78);
  for (uint8_t i = 0; i < routeCount(); ++i) {
    drawDirectoryRow(target,
                     freeink::ui::Rect{static_cast<int16_t>(popup.x + 16), y,
                                       static_cast<int16_t>(popup.width - 32), rowH},
                     routes[i].path, i == directoryIndex);
    y = static_cast<int16_t>(y + rowH + 4);
  }

  freeink::ui::TextStyle footer;
  footer.align = freeink::ui::TextAlign::Center;
  footer.maxLines = 1;
  freeink::ui::drawText(target,
                        freeink::ui::Rect{static_cast<int16_t>(popup.x + 16),
                                          static_cast<int16_t>(popup.bottom() - 32),
                                          static_cast<int16_t>(popup.width - 32), 24},
                        "Left/Right browse   Confirm select   Directory close", footer);
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

    pages[0] = mainPage;
    pages[1] = companionPage;
    pages[2] = companionStatsPage;
    pages[3] = settingsPage;
    pages[4] = otherTestPage;
    pages[5] = powerStatsPage;
    pages[6] = companionSettingsWarningPage;
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

PageId handlePageButton(ButtonPressKind kind) {
  const PageId page = activePage();

  xSemaphoreTake(pageMutex, portMAX_DELAY);
  const bool overlayOpen = directoryOpen;
  xSemaphoreGive(pageMutex);

  if (overlayOpen) {
    switch (kind) {
      case ButtonPressKind::Back:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        directoryOpen = false;
        xSemaphoreGive(pageMutex);
        return activePage();
      case ButtonPressKind::Left:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        moveDirectorySelection(-1);
        xSemaphoreGive(pageMutex);
        return activePage();
      case ButtonPressKind::Right:
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        moveDirectorySelection(1);
        xSemaphoreGive(pageMutex);
        return activePage();
      case ButtonPressKind::Confirm: {
        xSemaphoreTake(pageMutex, portMAX_DELAY);
        const PageId selected = routes[directoryIndex].id;
        directoryOpen = false;
        xSemaphoreGive(pageMutex);
        logPrintf("Directory selected: %s\n", routeFor(selected).path);
        return setCurrentPage(selected);
      }
      case ButtonPressKind::Up:
      case ButtonPressKind::Down:
      case ButtonPressKind::Gpio1:
      case ButtonPressKind::Gpio2:
      case ButtonPressKind::Power:
        return activePage();
    }
  }

  switch (kind) {
    case ButtonPressKind::Back:
      xSemaphoreTake(pageMutex, portMAX_DELAY);
      syncDirectorySelection(page);
      directoryOpen = true;
      xSemaphoreGive(pageMutex);
      return page;
    case ButtonPressKind::Up:
    case ButtonPressKind::Down:
      pageFor(page).handleButton(kind);
      return activePage();
    case ButtonPressKind::Left:
      pageFor(page).handleButton(kind);
      return activePage();
    case ButtonPressKind::Right:
      pageFor(page).handleButton(kind);
      return activePage();
    case ButtonPressKind::Confirm:
      pageFor(page).handleButton(kind);
      return activePage();
    case ButtonPressKind::Gpio1:
    case ButtonPressKind::Gpio2:
    case ButtonPressKind::Power:
      return activePage();
  }
  return activePage();
}

const char* pageName(PageId page) {
  return routeFor(page).path;
}

void drawPageChrome(freeink::ui::DisplayTarget& target) {
  const int16_t width = target.logicalWidth();
  const int16_t height = target.logicalHeight();

  freeink::ui::TextStyle left;
  left.align = freeink::ui::TextAlign::Left;
  left.maxLines = 1;

  freeink::ui::TextStyle right;
  right.align = freeink::ui::TextAlign::Right;
  right.maxLines = 1;

  char battery[20];
  if (batteryCached && batteryKnown) {
    snprintf(battery, sizeof(battery), "%u%%", batteryPercent);
  } else {
    snprintf(battery, sizeof(battery), "--%%");
  }

  target.fill(freeink::ui::Rect{0, 0, width, 38}, freeink::ui::Paint::solid(freeink::ui::Color::White));
  freeink::ui::drawText(target, freeink::ui::Rect{10, 2, static_cast<int16_t>(width - 130), 32},
                        "Nicholas.Maliwack@gmail.com", left);

  freeink::ui::InputSnapshot input;
  freeink::ui::InteractionBuffer<1> interactions;
  freeink::ui::Frame<1> frame(target, target.deviceContext(), input, interactions);
  freeink::ui::BatteryIndicatorProps batteryProps;
  batteryProps.percent = batteryKnown ? static_cast<uint8_t>(batteryPercent) : 0;
  batteryProps.label = battery;
  batteryProps.text = right;
  batteryProps.glyphWidth = 28;
  batteryProps.glyphHeight = 14;
  batteryProps.gap = 6;
  freeink::ui::batteryIndicator(frame, freeink::ui::Rect{static_cast<int16_t>(width - 122), 3, 112, 30},
                                batteryProps);

  const freeink::ui::Rect footer{0, static_cast<int16_t>(height - 34), width, 34};
  target.fill(footer, freeink::ui::Paint::solid(freeink::ui::Color::White));
  target.line(freeink::ui::Point{0, footer.y}, freeink::ui::Point{width, footer.y}, 1,
              freeink::ui::Paint::solid(freeink::ui::Color::Black));
  constexpr const char* defaultLabels[] = {"Directory", "Confirm", "Left", "Right"};
  constexpr const char* companionLabels[] = {"Directory", "Camera", "Mute", "Hand"};
  const char* const* labels = currentPage == PageId::Companion ? companionLabels : defaultLabels;
  const int16_t buttonW = static_cast<int16_t>(width / 4);
  freeink::ui::TextStyle buttonText;
  buttonText.align = freeink::ui::TextAlign::Center;
  buttonText.maxLines = 1;
  for (uint8_t i = 0; i < 4; ++i) {
    freeink::ui::drawText(target,
                          freeink::ui::Rect{static_cast<int16_t>(i * buttonW), static_cast<int16_t>(footer.y + 1),
                                            buttonW, 24},
                          labels[i], buttonText);
  }
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
  transaction.reset();
  page.postRender(*pageTarget, mode, forceDraw);
  xSemaphoreGive(pageMutex);
}

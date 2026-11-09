#pragma once

#include "IsrInput.h"
#include "PageId.h"

#include <Arduino.h>
#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>

struct PageButtonResult {
  PageId page = PageId::Main;
  bool renderRequired = true;
  bool overlayOnly = false;
  // Used when an overlay has been removed or the active page has changed. A
  // full refresh eliminates stale pixels and any partial-refresh ghosting.
  bool fullRefresh = false;
};

bool beginPageManager(EInkDisplay& display, PageId initialPage = PageId::Companion);
freeink::ui::DisplayTarget* pageDisplayTarget();
PageId activePage();
void drawPageChrome(freeink::ui::DisplayTarget& target);
// Re-reads the battery outside of a render. Returns true when the value now
// cached differs from what the page chrome last drew, so the caller should
// queue an active-page render to keep the on-screen battery current.
bool pollBatteryForChrome();
PageId showPage(PageId page);
PageId showNextPage();
PageId showPreviousPage();
PageButtonResult handlePageButton(ButtonPressKind kind);
PageButtonResult handlePageTouch(float panelX, float panelY);
const char* pageName(PageId page);
void renderActivePage(EInkDisplay::RefreshMode mode);
void renderDirectoryOverlay(EInkDisplay::RefreshMode mode);

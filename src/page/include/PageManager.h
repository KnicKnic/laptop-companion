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
};

bool beginPageManager(EInkDisplay& display, PageId initialPage = PageId::Companion);
freeink::ui::DisplayTarget* pageDisplayTarget();
PageId activePage();
void drawPageChrome(freeink::ui::DisplayTarget& target);
PageId showPage(PageId page);
PageId showNextPage();
PageId showPreviousPage();
PageButtonResult handlePageButton(ButtonPressKind kind);
const char* pageName(PageId page);
void renderActivePage(EInkDisplay::RefreshMode mode);
void renderDirectoryOverlay(EInkDisplay::RefreshMode mode);

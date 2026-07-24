#pragma once

#include "RenderSignal.h"

#include <EInkDisplay.h>

bool beginDisplayWorker(EInkDisplay& display);
bool displayWorkerReady();
uint32_t requestRender(RenderKind kind, EInkDisplay::RefreshMode mode);
uint32_t renderRequestCount();
bool waitForRender(uint32_t sequence, TickType_t timeoutTicks);

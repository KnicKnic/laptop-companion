#pragma once

#include "RenderSignal.h"

#include <EInkDisplay.h>

bool beginDisplayWorker(EInkDisplay& display);
bool displayWorkerReady();
uint32_t requestRender(RenderKind kind, EInkDisplay::RefreshMode mode);
bool waitForRender(uint32_t sequence, TickType_t timeoutTicks);

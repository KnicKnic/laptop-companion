#include "DisplayWorker.h"

#include "AppLog.h"
#include "PageManager.h"
#include "SleepScreen.h"
#include "SleepImage.h"

#include <Arduino.h>
#include <freertos/task.h>

namespace {

EInkDisplay* display = nullptr;
TaskHandle_t displayTaskHandle = nullptr;
SequencedRenderSignal renderSignal;
constexpr UBaseType_t kDisplayTaskPriority = 1;

void markRenderComplete(uint32_t sequence) {
  renderSignal.completeWork(sequence);
}

void renderSleepScreenAndShutdownDisplay(uint32_t sequence) {
  logPrintf("Display task: drawing SD sleep image.\n");
  bool sleepImagePresented = false;
  if (!drawSleepImageFromSd(*display, &sleepImagePresented)) {
    logPrintf("Display task: falling back to built-in static sleep image.\n");
    freeink::ui::DisplayTarget* target = pageDisplayTarget();
    if (target != nullptr) {
      drawStaticSleepImage(*display, *target);
    }
  }

  if (!sleepImagePresented) {
    display->displayBuffer(EInkDisplay::FULL_REFRESH);
  }
  display->deepSleep();
  markRenderComplete(sequence);
}

void displayTask(void*) {
  for (;;) {
    const RenderRequest request = renderSignal.waitForWork();

    if (request.kind == RenderKind::Sleep) {
      renderSleepScreenAndShutdownDisplay(request.sequence);
      continue;
    }

    renderActivePage(request.mode);
    markRenderComplete(request.sequence);
  }
}

}  // namespace

bool beginDisplayWorker(EInkDisplay& displayRef) {
  display = &displayRef;
  if (!renderSignal.begin()) return false;
  xTaskCreate(displayTask, "display", 8192, nullptr, kDisplayTaskPriority, &displayTaskHandle);
  return displayTaskHandle != nullptr;
}

bool displayWorkerReady() {
  return displayTaskHandle != nullptr;
}

uint32_t requestRender(RenderKind kind, EInkDisplay::RefreshMode mode) {
  RenderRequest request;
  request.kind = kind;
  request.mode = mode;
  return renderSignal.signalWork(request);
}

bool waitForRender(uint32_t sequence, TickType_t timeoutTicks) {
  return renderSignal.waitForAtLeast(sequence, timeoutTicks);
}

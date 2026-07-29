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
constexpr uint32_t kDisplayTaskStackBytes = 12288;

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

    switch (request.kind) {
      case RenderKind::Sleep:
        renderSleepScreenAndShutdownDisplay(request.sequence);
        continue;
      case RenderKind::DirectoryOverlay:
        renderDirectoryOverlay(request.mode);
        break;
      case RenderKind::ActivePage:
        renderActivePage(request.mode);
        break;
    }
    markRenderComplete(request.sequence);
  }
}

}  // namespace

bool beginDisplayWorker(EInkDisplay& displayRef) {
  display = &displayRef;
  if (!renderSignal.begin()) return false;
  xTaskCreate(displayTask, "display", kDisplayTaskStackBytes, nullptr, kDisplayTaskPriority, &displayTaskHandle);
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

uint32_t renderRequestCount() {
  return renderSignal.latestSequence();
}

bool waitForRender(uint32_t sequence, TickType_t timeoutTicks) {
  return renderSignal.waitForAtLeast(sequence, timeoutTicks);
}

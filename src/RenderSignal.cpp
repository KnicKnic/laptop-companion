#include "RenderSignal.h"

#include <freertos/task.h>

bool SequencedRenderSignal::begin() {
  mutex_ = xSemaphoreCreateMutex();
  completionMutex_ = xSemaphoreCreateMutex();
  workAvailable_ =
      xQueueCreateStatic(kWorkAvailableQueueDepth, sizeof(uint8_t), workAvailableStorage_, &workAvailableQueueBuffer_);
  return mutex_ != nullptr && completionMutex_ != nullptr && workAvailable_ != nullptr;
}

uint32_t SequencedRenderSignal::signalWork(RenderRequest request) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  request.sequence = latestRequest_.sequence + 1;
  latestRequest_ = request;
  xSemaphoreGive(mutex_);

  const uint8_t token = 1;
  xQueueSend(workAvailable_, &token, 0);
  return request.sequence;
}

RenderRequest SequencedRenderSignal::waitForWork() {
  for (;;) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (latestRequest_.sequence > claimedSequence_) {
      claimedSequence_ = latestRequest_.sequence;
      RenderRequest request = latestRequest_;
      xSemaphoreGive(mutex_);
      drainWorkAvailableQueue();
      return request;
    }
    xSemaphoreGive(mutex_);

    uint8_t token = 0;
    xQueueReceive(workAvailable_, &token, portMAX_DELAY);
  }
}

void SequencedRenderSignal::drainWorkAvailableQueue() {
  uint8_t token = 0;
  while (xQueueReceive(workAvailable_, &token, 0) == pdTRUE) {
  }
}

void SequencedRenderSignal::completeWork(uint32_t sequence) {
  xSemaphoreTake(completionMutex_, portMAX_DELAY);
  if (sequence > completedSequence_) {
    completedSequence_ = sequence;
  }
  xSemaphoreGive(completionMutex_);
}

bool SequencedRenderSignal::waitForAtLeast(uint32_t sequence, TickType_t timeoutTicks) {
  const TickType_t startedAt = xTaskGetTickCount();
  for (;;) {
    xSemaphoreTake(completionMutex_, portMAX_DELAY);
    const bool done = completedSequence_ >= sequence;
    if (done) {
      xSemaphoreGive(completionMutex_);
      return true;
    }

    if (timeoutTicks != portMAX_DELAY && (xTaskGetTickCount() - startedAt) >= timeoutTicks) {
      xSemaphoreGive(completionMutex_);
      return false;
    }
    xSemaphoreGive(completionMutex_);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

uint32_t SequencedRenderSignal::latestSequence() const {
  if (mutex_ == nullptr) {
    return latestRequest_.sequence;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  const uint32_t sequence = latestRequest_.sequence;
  xSemaphoreGive(mutex_);
  return sequence;
}

#pragma once

#include <Arduino.h>
#include <EInkDisplay.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

enum class RenderKind : uint8_t {
  ActivePage,
  Sleep,
};

struct RenderRequest {
  RenderKind kind = RenderKind::ActivePage;
  EInkDisplay::RefreshMode mode = EInkDisplay::FULL_REFRESH;
  uint32_t sequence = 0;
};

class SequencedRenderSignal {
 public:
  bool begin();
  uint32_t signalWork(RenderRequest request);
  RenderRequest waitForWork();
  void completeWork(uint32_t sequence);
  bool waitForAtLeast(uint32_t sequence, TickType_t timeoutTicks);

 private:
  static constexpr UBaseType_t kWorkAvailableQueueDepth = 20;
  void drainWorkAvailableQueue();

  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t completionMutex_ = nullptr;
  QueueHandle_t workAvailable_ = nullptr;
  StaticQueue_t workAvailableQueueBuffer_{};
  uint8_t workAvailableStorage_[kWorkAvailableQueueDepth * sizeof(uint8_t)] = {};
  RenderRequest latestRequest_{};
  uint32_t claimedSequence_ = 0;
  uint32_t completedSequence_ = 0;
};

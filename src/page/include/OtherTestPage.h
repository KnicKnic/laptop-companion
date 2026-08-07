#pragma once

#include "Page.h"

#include <atomic>
#include <cstdint>

class OtherTestPage final : public Page {
 public:
  explicit OtherTestPage(EInkDisplay& display);

  PageId id() const override;
  const char* name() const override;
  void onEnter() override;
  void onLeave() override;
  std::unique_ptr<RenderTransaction> render(freeink::ui::DisplayTarget& target,
                                            EInkDisplay::RefreshMode mode, bool forceDraw) override;

 private:
  enum class CrystalTestState : uint8_t {
    Idle,
    WaitingToStart,
    Measuring,
    Passed,
    Failed,
  };

  static constexpr uint8_t kCalibrationSamples = 3;

  static void crystalTestTask(void* context);
  void runCrystalTest();
  void requestCrystalTestRender() const;

  std::atomic<CrystalTestState> crystalTestState_{CrystalTestState::Idle};
  std::atomic<uint32_t> crystalTestGeneration_{0};
  std::atomic<bool> crystalTestRequested_{false};
  std::atomic<bool> crystalTestTaskRunning_{false};
  std::atomic<uint32_t> calibrationValues_[kCalibrationSamples]{};
};

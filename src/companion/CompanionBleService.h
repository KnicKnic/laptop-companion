#pragma once

#include "CompanionProtocol.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <string>

class NimBLECharacteristic;
class NimBLEServer;

class CompanionBleService {
 public:
  using StatusChangedCallback = void (*)();

  enum class ConnectionPowerProfile : uint8_t {
    Unknown,
    Responsive,
    Idle,
  };

  struct HostStatus {
    bool teamsDetected = false;
    uint16_t teamsCounter = 0;
    bool meetingDetected = false;
    uint16_t meetingCounter = 0;
    std::string meetingName;
    uint8_t microphone = 0;
    uint16_t microphoneCounter = 0;
    uint8_t camera = 0;
    uint16_t cameraCounter = 0;
    bool cameraLocked = false;
    uint8_t hand = 0;
    uint16_t handCounter = 0;
    bool handLocked = false;
    std::string message;
  };

  struct DesktopInfo {
    std::string name;
    bool remote = false;
    bool disconnected = false;
  };

  struct DesktopStatus {
    bool valid = false;
    uint8_t count = 0;
    uint8_t activeIndex = CompanionProtocol::DESKTOP_INDEX_UNKNOWN;
    DesktopInfo desktops[CompanionProtocol::DESKTOP_MAX_COUNT];
  };

  struct PendingButtonStatus {
    bool mutePending = false;
    uint16_t muteCounter = 0;
    uint32_t mutePressedAtMs = 0;
    bool handPending = false;
    uint16_t handCounter = 0;
    uint32_t handPressedAtMs = 0;
    bool cameraPending = false;
    uint16_t cameraCounter = 0;
    uint32_t cameraPressedAtMs = 0;
    bool desktopPending = false;
    uint16_t desktopCounter = 0;
    uint32_t desktopPressedAtMs = 0;
    uint8_t desktopTargetIndex = CompanionProtocol::DESKTOP_INDEX_UNKNOWN;
    // Reactions carry no state, so this only spans the BLE handover plus a short minimum
    // hold: it is set as the press is published and cleared once the notification has gone
    // out and the styling has been visible long enough to see.
    bool reactionPending = false;
    bool reactionTransmitted = false;
    uint8_t reactionButtonId = 0;
    uint32_t reactionPressedAtMs = 0;
    bool lastAcknowledgedValid = false;
    uint8_t lastAcknowledgedButtonId = 0;
    uint16_t lastAcknowledgedCounter = 0;
    uint32_t lastAcknowledgedLatencyMs = 0;
  };

  struct ActivityStats {
    uint32_t updateCalls = 0;
    uint32_t maintenanceRuns = 0;
    uint32_t gapConnects = 0;
    uint32_t gapDisconnects = 0;
    uint32_t connParamRequests = 0;
    uint32_t connParamUpdates = 0;
    uint32_t hostWrites = 0;
    uint32_t hostStateChanges = 0;
    uint32_t buttonSubscribes = 0;
    uint32_t buttonNotifications = 0;
    uint32_t participationTimerChecks = 0;
    uint32_t participationSubscribes = 0;
    uint32_t participationNotificationAttempts = 0;
    uint32_t participationNotifications = 0;
    uint32_t participationNotificationFailures = 0;
    uint32_t advertisingRestarts = 0;
  };

  static CompanionBleService& getInstance();

  bool begin();
  void end();
  void update();
  void setStatusChangedCallback(StatusChangedCallback callback);

  bool isRunning() const { return running_; }
  bool isHostConnected() const { return hostConnected_; }
  bool isAdvertising() const;
  bool isButtonSubscribed() const { return buttonEventSubscribed_; }
  bool notifyToggleMuteReleased(uint16_t* counter = nullptr);
  bool notifyToggleHandReleased(uint16_t* counter = nullptr);
  bool notifyToggleCameraReleased(uint16_t* counter = nullptr);
  bool notifySwitchDesktopReleased(uint8_t desktopIndex, uint16_t* counter = nullptr);
  bool notifyReactionReleased(CompanionProtocol::ButtonId reaction, uint16_t* counter = nullptr);
  std::string getStatusText() const;
  HostStatus getHostStatus() const;
  DesktopStatus getDesktopStatus() const;
  PendingButtonStatus getPendingButtonStatus() const;
  ActivityStats getActivityStats() const;
  uint32_t getBluetoothSessionRenderRequests() const;
  std::string formatTimingDiagnostics() const;
  std::string formatActivityDeltaDiagnostics();

  void onHostConnected(uint16_t connHandle);
  void onHostDisconnected();
  void onConnParamsUpdated(uint16_t interval, uint16_t latency, uint16_t timeout);
  void onHostTeamsStateWritten(NimBLECharacteristic* characteristic);
  void onHostMeetingStateWritten(NimBLECharacteristic* characteristic);
  void onHostMeetingNameWritten(NimBLECharacteristic* characteristic);
  void onHostMicrophoneStateWritten(NimBLECharacteristic* characteristic);
  void onHostCameraStateWritten(NimBLECharacteristic* characteristic);
  void onHostHandStateWritten(NimBLECharacteristic* characteristic);
  void onHostStatusMessageWritten(NimBLECharacteristic* characteristic);
  void onHostDesktopStateWritten(NimBLECharacteristic* characteristic);
  void onButtonEventSubscribed(bool subscribed);
  void onParticipationSubscribed(bool subscribed);

 private:
  CompanionBleService() = default;

  void resetSessionState();
  void publishHostStateValues();
  void publishDeviceInfo();
  bool publishButtonEvent(uint8_t buttonId, uint8_t action, uint8_t argument, uint16_t* counter);
  void publishParticipationEventIfDue();
  void requestConnectionParams(ConnectionPowerProfile profile, const char* reason);
  void requestIdleConnectionParamsIfReady(const char* reason);
  bool restartAdvertising(const char* reason);
  StatusChangedCallback settlePendingButtonsLocked(unsigned long now);
  StatusChangedCallback markStatusChangedLocked();
  void markStatusChanged();
  void notifyStatusChanged(StatusChangedCallback callback) const;
  bool ensureStateMutex();
  void lockState() const;
  void unlockState() const;
  void startWorker();
  void stopWorker();
  static void workerTrampoline(void* self);
  void workerLoop();

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* hostTeamsStateCharacteristic_ = nullptr;
  NimBLECharacteristic* hostMeetingStateCharacteristic_ = nullptr;
  NimBLECharacteristic* hostMeetingNameCharacteristic_ = nullptr;
  NimBLECharacteristic* hostMicrophoneStateCharacteristic_ = nullptr;
  NimBLECharacteristic* hostCameraStateCharacteristic_ = nullptr;
  NimBLECharacteristic* hostHandStateCharacteristic_ = nullptr;
  NimBLECharacteristic* hostStatusMessageCharacteristic_ = nullptr;
  NimBLECharacteristic* hostDesktopStateCharacteristic_ = nullptr;
  NimBLECharacteristic* buttonEventCharacteristic_ = nullptr;
  NimBLECharacteristic* participationCharacteristic_ = nullptr;
  NimBLECharacteristic* deviceInfoCharacteristic_ = nullptr;
  TaskHandle_t workerTask_ = nullptr;
  mutable SemaphoreHandle_t stateMutex_ = nullptr;
  bool workerStopRequested_ = false;

  bool running_ = false;
  bool hostConnected_ = false;
  bool hostStateReceived_ = false;
  bool buttonEventSubscribed_ = false;
  bool participationSubscribed_ = false;
  bool ownsBluetoothStack_ = false;
  bool modemSleepEnabled_ = false;
  bool statusChanged_ = false;
  ConnectionPowerProfile connectionProfile_ = ConnectionPowerProfile::Unknown;
  ConnectionPowerProfile requestedConnectionProfile_ = ConnectionPowerProfile::Unknown;
  uint16_t hostConnHandle_ = 0xFFFF;
  uint16_t requestedConnIntervalMin_ = 0;
  uint16_t requestedConnIntervalMax_ = 0;
  uint16_t requestedConnLatency_ = 0;
  uint16_t requestedConnTimeout_ = 0;
  uint16_t negotiatedConnInterval_ = 0;
  uint16_t negotiatedConnLatency_ = 0;
  uint16_t negotiatedConnTimeout_ = 0;
  unsigned long hostConnectedAtMs_ = 0;
  unsigned long lastMaintenanceAtMs_ = 0;
  unsigned long lastAdvertisingRestartAtMs_ = 0;
  unsigned long lastConnParamRequestAtMs_ = 0;
  unsigned long responsiveUntilMs_ = 0;
  unsigned long participationUntilMs_ = 0;
  unsigned long lastParticipationNotifyAtMs_ = 0;
  bool hasNegotiatedConnParams_ = false;
  uint16_t buttonEventSequence_ = 0;
  uint32_t participationCounter_ = 0;
  uint32_t bluetoothSessionRenderBaseline_ = 0;
  HostStatus hostStatus_;
  DesktopStatus desktopStatus_;
  PendingButtonStatus pendingButtons_;
  ActivityStats activityStats_;
  ActivityStats previousActivityStats_;
  bool hasPreviousActivityStats_ = false;
  StatusChangedCallback statusChangedCallback_ = nullptr;
};

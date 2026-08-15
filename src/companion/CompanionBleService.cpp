#include "CompanionBleService.h"

#include "AppLog.h"
#include "CompanionProtocol.h"
#include "DisplayWorker.h"
#include "PowerStats.h"

#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <esp_bt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr unsigned long kMaintenanceIntervalMs = 2000;
constexpr unsigned long kHandshakeTimeoutMs = 15000;
constexpr unsigned long kAdvertisingRestartIntervalMs = 5000;
constexpr unsigned long kConnParamRequestMinIntervalMs = 2500;
constexpr unsigned long kButtonResponsiveWindowMs = 5000;
constexpr unsigned long kParticipationWindowMs = 5000;
constexpr unsigned long kWorkerPollMs = 500;
constexpr unsigned long kWorkerPollActiveMs = 25;
constexpr size_t kStatusMessageMaxLen = 48;
constexpr size_t kEncodedStateLen = 2;

constexpr uint16_t kConnIntervalResponsiveMin = 24;  // 30 ms
constexpr uint16_t kConnIntervalResponsiveMax = 40;  // 50 ms
constexpr uint16_t kConnLatencyResponsive = 0;
constexpr uint16_t kConnTimeoutResponsive = 400;     // 4 s

constexpr uint16_t kConnIntervalIdleMin = 80;        // 100 ms
constexpr uint16_t kConnIntervalIdleMax = 96;        // 120 ms
constexpr uint16_t kConnLatencyIdle = 10;
constexpr uint16_t kConnTimeoutIdle = 1500;          // 15 s
constexpr uint16_t kAdvIntervalLowPowerMin = 800;    // 500 ms
constexpr uint16_t kAdvIntervalLowPowerMax = 1600;   // 1000 ms

enum class HostStateField : uint8_t {
  Teams,
  Meeting,
  MeetingName,
  Microphone,
  Camera,
  Hand,
  Message,
  Desktops,
};

CompanionBleService* g_service = nullptr;

const char* profileName(CompanionBleService::ConnectionPowerProfile profile) {
  switch (profile) {
    case CompanionBleService::ConnectionPowerProfile::Responsive:
      return "responsive";
    case CompanionBleService::ConnectionPowerProfile::Idle:
      return "idle";
    default:
      return "unknown";
  }
}

uint32_t connIntervalTenthsMs(uint16_t interval) {
  return (static_cast<uint32_t>(interval) * 125U + 5U) / 10U;
}

uint32_t advIntervalTenthsMs(uint16_t interval) {
  return (static_cast<uint32_t>(interval) * 625U + 50U) / 100U;
}

void formatTenthsMs(uint32_t tenthsMs, char* buffer, size_t bufferSize) {
  if (tenthsMs % 10U == 0) {
    snprintf(buffer, bufferSize, "%lums", static_cast<unsigned long>(tenthsMs / 10U));
    return;
  }
  snprintf(buffer, bufferSize, "%lu.%lums", static_cast<unsigned long>(tenthsMs / 10U),
           static_cast<unsigned long>(tenthsMs % 10U));
}

uint32_t deltaCounter(uint32_t current, uint32_t previous) {
  return current >= previous ? current - previous : 0;
}

uint16_t nextButtonCounter(uint16_t current) {
  uint16_t next = static_cast<uint16_t>((current + 1U) & CompanionProtocol::STATE_COUNTER_MASK);
  return next == 0 ? 1 : next;
}

uint16_t decodeLe16(const std::string& value) {
  return static_cast<uint16_t>(static_cast<uint8_t>(value[0])) |
         static_cast<uint16_t>(static_cast<uint8_t>(value[1]) << 8);
}

bool decodeBoolState(const std::string& value, bool* on, uint16_t* counter, bool* locked = nullptr) {
  if (value.size() != kEncodedStateLen || !on || !counter) return false;
  const uint16_t encoded = decodeLe16(value);
  *on = (encoded & 0x0001U) != 0;
  if (locked) *locked = (encoded & 0x0002U) != 0;
  *counter = static_cast<uint16_t>(encoded >> 2);
  return true;
}

bool decodeTriState(const std::string& value, uint8_t* state, uint16_t* counter, bool* locked = nullptr) {
  if (value.size() != kEncodedStateLen || !state || !counter) return false;
  const uint16_t encoded = decodeLe16(value);
  *state = (encoded & 0x0001U) != 0 ? static_cast<uint8_t>(CompanionProtocol::TriState::On)
                                    : static_cast<uint8_t>(CompanionProtocol::TriState::Off);
  if (locked) *locked = (encoded & 0x0002U) != 0;
  *counter = static_cast<uint16_t>(encoded >> 2);
  return true;
}

void setEncodedStateValue(NimBLECharacteristic* characteristic, bool on, uint16_t counter, bool locked = false) {
  if (!characteristic) return;
  const uint16_t encoded = CompanionProtocol::encodeState(on, counter, locked);
  const uint8_t payload[] = {
      static_cast<uint8_t>(encoded & 0xFF),
      static_cast<uint8_t>((encoded >> 8) & 0xFF),
  };
  characteristic->setValue(payload, sizeof(payload));
}

// Decodes [version][count][activeIndex][ackSeqLo][ackSeqHi] followed by [flags][nameLen][name]
// per desktop.
bool decodeDesktopState(const std::string& value, CompanionBleService::DesktopStatus* out,
                        uint16_t* ackSequence) {
  if (!out || value.size() < CompanionProtocol::DESKTOP_STATE_HEADER_LEN) return false;
  if (static_cast<uint8_t>(value[0]) != CompanionProtocol::PROTOCOL_VERSION) return false;

  const uint8_t count = static_cast<uint8_t>(value[1]);
  if (count > CompanionProtocol::DESKTOP_MAX_COUNT) return false;

  CompanionBleService::DesktopStatus next;
  next.valid = true;
  next.count = count;
  next.activeIndex = static_cast<uint8_t>(value[2]);
  if (next.activeIndex != CompanionProtocol::DESKTOP_INDEX_UNKNOWN && next.activeIndex >= count) {
    next.activeIndex = CompanionProtocol::DESKTOP_INDEX_UNKNOWN;
  }
  if (ackSequence) {
    *ackSequence = static_cast<uint16_t>(static_cast<uint8_t>(value[3])) |
                   static_cast<uint16_t>(static_cast<uint8_t>(value[4]) << 8);
  }

  size_t offset = CompanionProtocol::DESKTOP_STATE_HEADER_LEN;
  for (uint8_t i = 0; i < count; ++i) {
    if (offset + 2 > value.size()) return false;
    const uint8_t flags = static_cast<uint8_t>(value[offset]);
    const uint8_t nameLen = static_cast<uint8_t>(value[offset + 1]);
    offset += 2;
    if (nameLen > CompanionProtocol::DESKTOP_NAME_MAX_LEN || offset + nameLen > value.size()) return false;
    next.desktops[i].remote = (flags & CompanionProtocol::DESKTOP_FLAG_REMOTE) != 0;
    next.desktops[i].disconnected = (flags & CompanionProtocol::DESKTOP_FLAG_DISCONNECTED) != 0;
    next.desktops[i].name.assign(value, offset, nameLen);
    offset += nameLen;
  }

  *out = next;
  return true;
}

bool desktopStatusEquals(const CompanionBleService::DesktopStatus& a, const CompanionBleService::DesktopStatus& b) {
  if (a.valid != b.valid || a.count != b.count || a.activeIndex != b.activeIndex) return false;
  for (uint8_t i = 0; i < a.count; ++i) {
    if (a.desktops[i].remote != b.desktops[i].remote || a.desktops[i].name != b.desktops[i].name ||
        a.desktops[i].disconnected != b.desktops[i].disconnected) {
      return false;
    }
  }
  return true;
}

std::string encodeDesktopState(const CompanionBleService::DesktopStatus& status) {
  std::string payload;
  payload.reserve(CompanionProtocol::DESKTOP_STATE_MAX_LEN);
  payload.push_back(static_cast<char>(CompanionProtocol::PROTOCOL_VERSION));
  payload.push_back(static_cast<char>(status.valid ? status.count : 0));
  payload.push_back(static_cast<char>(status.valid ? status.activeIndex : CompanionProtocol::DESKTOP_INDEX_UNKNOWN));
  payload.push_back(static_cast<char>(CompanionProtocol::DESKTOP_ACK_NONE & 0xFF));
  payload.push_back(static_cast<char>((CompanionProtocol::DESKTOP_ACK_NONE >> 8) & 0xFF));
  if (!status.valid) return payload;

  for (uint8_t i = 0; i < status.count; ++i) {
    const std::string& name = status.desktops[i].name;
    const uint8_t nameLen = static_cast<uint8_t>(
        name.size() > CompanionProtocol::DESKTOP_NAME_MAX_LEN ? CompanionProtocol::DESKTOP_NAME_MAX_LEN : name.size());
    const uint8_t flags = static_cast<uint8_t>(
        (status.desktops[i].remote ? CompanionProtocol::DESKTOP_FLAG_REMOTE : 0) |
        (status.desktops[i].disconnected ? CompanionProtocol::DESKTOP_FLAG_DISCONNECTED : 0));
    payload.push_back(static_cast<char>(flags));
    payload.push_back(static_cast<char>(nameLen));
    payload.append(name, 0, nameLen);
  }
  return payload;
}

class CompanionServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    logPrintf("Companion host connected handle=%u\n", static_cast<unsigned>(connInfo.getConnHandle()));
    if (g_service) {
      g_service->onHostConnected(connInfo.getConnHandle());
      g_service->onConnParamsUpdated(connInfo.getConnInterval(), connInfo.getConnLatency(), connInfo.getConnTimeout());
    }
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& connInfo, int reason) override {
    logPrintf("Companion host disconnected handle=%u reason=%d\n",
              static_cast<unsigned>(connInfo.getConnHandle()), reason);
    if (g_service) {
      g_service->onHostDisconnected();
    }
  }

  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    if (g_service) {
      g_service->onConnParamsUpdated(connInfo.getConnInterval(), connInfo.getConnLatency(), connInfo.getConnTimeout());
    }
  }
};

class HostStateCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit HostStateCallbacks(HostStateField field) : field_(field) {}

 private:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    if (!g_service) return;
    switch (field_) {
      case HostStateField::Teams:
        g_service->onHostTeamsStateWritten(characteristic);
        break;
      case HostStateField::Meeting:
        g_service->onHostMeetingStateWritten(characteristic);
        break;
      case HostStateField::MeetingName:
        g_service->onHostMeetingNameWritten(characteristic);
        break;
      case HostStateField::Microphone:
        g_service->onHostMicrophoneStateWritten(characteristic);
        break;
      case HostStateField::Camera:
        g_service->onHostCameraStateWritten(characteristic);
        break;
      case HostStateField::Hand:
        g_service->onHostHandStateWritten(characteristic);
        break;
      case HostStateField::Message:
        g_service->onHostStatusMessageWritten(characteristic);
        break;
      case HostStateField::Desktops:
        g_service->onHostDesktopStateWritten(characteristic);
        break;
    }
  }

  HostStateField field_;
};

class ButtonEventCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    if (g_service) {
      g_service->onButtonEventSubscribed(subValue != 0);
    }
  }
};

class ParticipationCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    if (g_service) {
      g_service->onParticipationSubscribed(subValue != 0);
    }
  }
};

CompanionServerCallbacks serverCallbacks;
HostStateCallbacks teamsStateCallbacks(HostStateField::Teams);
HostStateCallbacks meetingStateCallbacks(HostStateField::Meeting);
HostStateCallbacks meetingNameCallbacks(HostStateField::MeetingName);
HostStateCallbacks microphoneStateCallbacks(HostStateField::Microphone);
HostStateCallbacks cameraStateCallbacks(HostStateField::Camera);
HostStateCallbacks handStateCallbacks(HostStateField::Hand);
HostStateCallbacks statusMessageCallbacks(HostStateField::Message);
HostStateCallbacks desktopStateCallbacks(HostStateField::Desktops);
ButtonEventCallbacks buttonEventCallbacks;
ParticipationCallbacks participationCallbacks;
}  // namespace

CompanionBleService& CompanionBleService::getInstance() {
  static CompanionBleService service;
  return service;
}

bool CompanionBleService::begin() {
  if (running_) return true;
  if (!ensureStateMutex()) {
    logPrintf("Companion BLE: failed to create state mutex\n");
    return false;
  }

  resetSessionState();
  activityStats_ = ActivityStats{};
  previousActivityStats_ = ActivityStats{};
  hasPreviousActivityStats_ = false;

  ownsBluetoothStack_ = !NimBLEDevice::isInitialized();
  if (!NimBLEDevice::isInitialized() && !NimBLEDevice::init("X3 Companion")) {
    logPrintf("Companion BLE: NimBLE init failed\n");
    markStatusChanged();
    return false;
  }

#if defined(CONFIG_BT_CTRL_MODEM_SLEEP) && CONFIG_BT_CTRL_MODEM_SLEEP
  const esp_err_t sleepErr = esp_bt_sleep_enable();
  modemSleepEnabled_ = sleepErr == ESP_OK;
  logPrintf("Companion BLE modem sleep %s (%d)\n", modemSleepEnabled_ ? "enabled" : "failed", sleepErr);
#endif

  g_service = this;
  server_ = NimBLEDevice::createServer();
  if (!server_) {
    logPrintf("Companion BLE: failed to create server\n");
    end();
    return false;
  }

  server_->setCallbacks(&serverCallbacks, false);
  NimBLEService* service = server_->createService(CompanionProtocol::SERVICE_UUID);
  if (!service) {
    logPrintf("Companion BLE: failed to create GATT service\n");
    end();
    return false;
  }

  constexpr uint32_t stateProperties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
  hostTeamsStateCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_TEAMS_STATE_UUID, stateProperties, kEncodedStateLen);
  hostMeetingStateCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_MEETING_STATE_UUID, stateProperties, kEncodedStateLen);
  hostMeetingNameCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_MEETING_NAME_UUID, stateProperties, kStatusMessageMaxLen);
  hostMicrophoneStateCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_MICROPHONE_STATE_UUID, stateProperties, kEncodedStateLen);
  hostCameraStateCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_CAMERA_STATE_UUID, stateProperties, kEncodedStateLen);
  hostHandStateCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_HAND_STATE_UUID, stateProperties, kEncodedStateLen);
  hostStatusMessageCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::HOST_STATUS_MESSAGE_UUID, stateProperties, kStatusMessageMaxLen);
  hostDesktopStateCharacteristic_ = service->createCharacteristic(
      CompanionProtocol::HOST_DESKTOP_STATE_UUID, stateProperties, CompanionProtocol::DESKTOP_STATE_MAX_LEN);
  buttonEventCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::BUTTON_EVENT_UUID,
                                    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                    CompanionProtocol::BUTTON_EVENT_PAYLOAD_LEN);
  participationCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::CONNECTION_PARTICIPATION_UUID,
                                    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 5);
  deviceInfoCharacteristic_ =
      service->createCharacteristic(CompanionProtocol::DEVICE_INFO_UUID, NIMBLE_PROPERTY::READ, 2);

  if (!hostTeamsStateCharacteristic_ || !hostMeetingStateCharacteristic_ || !hostMeetingNameCharacteristic_ ||
      !hostMicrophoneStateCharacteristic_ || !hostCameraStateCharacteristic_ || !hostHandStateCharacteristic_ ||
      !hostStatusMessageCharacteristic_ || !buttonEventCharacteristic_ || !participationCharacteristic_ ||
      !deviceInfoCharacteristic_ || !hostDesktopStateCharacteristic_) {
    logPrintf("Companion BLE: failed to create characteristics\n");
    end();
    return false;
  }

  hostTeamsStateCharacteristic_->setCallbacks(&teamsStateCallbacks);
  hostMeetingStateCharacteristic_->setCallbacks(&meetingStateCallbacks);
  hostMeetingNameCharacteristic_->setCallbacks(&meetingNameCallbacks);
  hostMicrophoneStateCharacteristic_->setCallbacks(&microphoneStateCallbacks);
  hostCameraStateCharacteristic_->setCallbacks(&cameraStateCallbacks);
  hostHandStateCharacteristic_->setCallbacks(&handStateCallbacks);
  hostStatusMessageCharacteristic_->setCallbacks(&statusMessageCallbacks);
  hostDesktopStateCharacteristic_->setCallbacks(&desktopStateCallbacks);
  buttonEventCharacteristic_->setCallbacks(&buttonEventCallbacks);
  participationCharacteristic_->setCallbacks(&participationCallbacks);

  if (!server_->start()) {
    logPrintf("Companion BLE: failed to start GATT server\n");
    end();
    return false;
  }

  publishHostStateValues();
  publishDeviceInfo();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->clearData();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(CompanionProtocol::SERVICE_UUID);
  advertising->setMinInterval(kAdvIntervalLowPowerMin);
  advertising->setMaxInterval(kAdvIntervalLowPowerMax);
  advertising->setPreferredParams(kConnIntervalIdleMin, kConnIntervalIdleMax);
  advertising->setName("X3 Companion");
  if (!advertising->start()) {
    logPrintf("Companion BLE: failed to start advertising\n");
    end();
    return false;
  }

  running_ = true;
  lastMaintenanceAtMs_ = millis();
  lastAdvertisingRestartAtMs_ = 0;
  markStatusChanged();
  startWorker();
  logPrintf("Companion BLE GATT server started\n");
  return true;
}

void CompanionBleService::end() {
  stopWorker();
  g_service = nullptr;

  if (running_) {
    NimBLEDevice::stopAdvertising();
  }

  running_ = false;
  resetSessionState();
  markStatusChanged();

#if defined(CONFIG_BT_CTRL_MODEM_SLEEP) && CONFIG_BT_CTRL_MODEM_SLEEP
  if (modemSleepEnabled_) {
    esp_bt_sleep_disable();
    modemSleepEnabled_ = false;
  }
#endif

  if (ownsBluetoothStack_ && NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(true);
  }

  server_ = nullptr;
  hostTeamsStateCharacteristic_ = nullptr;
  hostMeetingStateCharacteristic_ = nullptr;
  hostMeetingNameCharacteristic_ = nullptr;
  hostMicrophoneStateCharacteristic_ = nullptr;
  hostCameraStateCharacteristic_ = nullptr;
  hostHandStateCharacteristic_ = nullptr;
  hostStatusMessageCharacteristic_ = nullptr;
  hostDesktopStateCharacteristic_ = nullptr;
  buttonEventCharacteristic_ = nullptr;
  participationCharacteristic_ = nullptr;
  deviceInfoCharacteristic_ = nullptr;
  ownsBluetoothStack_ = false;
}

void CompanionBleService::setStatusChangedCallback(StatusChangedCallback callback) {
  if (!ensureStateMutex()) return;

  lockState();
  statusChangedCallback_ = callback;
  unlockState();
}

bool CompanionBleService::isAdvertising() const {
  lockState();
  const bool running = running_;
  const bool connected = hostConnected_;
  unlockState();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  return running && !connected && advertising && advertising->isAdvertising();
}

std::string CompanionBleService::getStatusText() const {
  lockState();
  std::string result;
  if (!running_) {
    result = "BLE stopped";
  } else if (hostConnected_) {
    if (!hostStatus_.message.empty()) {
      result = hostStatus_.message;
    } else {
      result = buttonEventSubscribed_ ? "Host connected" : "Host connected, waiting";
    }
  } else {
    result = isAdvertising() ? "Advertising" : "Advertising paused";
  }
  unlockState();
  return result;
}

CompanionBleService::HostStatus CompanionBleService::getHostStatus() const {
  lockState();
  const HostStatus status = hostStatus_;
  unlockState();
  return status;
}

CompanionBleService::DesktopStatus CompanionBleService::getDesktopStatus() const {
  lockState();
  const DesktopStatus status = desktopStatus_;
  unlockState();
  return status;
}

CompanionBleService::PendingButtonStatus CompanionBleService::getPendingButtonStatus() const {
  lockState();
  const PendingButtonStatus status = pendingButtons_;
  unlockState();
  return status;
}

CompanionBleService::ActivityStats CompanionBleService::getActivityStats() const {
  lockState();
  const ActivityStats stats = activityStats_;
  unlockState();
  return stats;
}

uint32_t CompanionBleService::getBluetoothSessionRenderRequests() const {
  lockState();
  const bool connected = hostConnected_;
  const uint32_t baseline = bluetoothSessionRenderBaseline_;
  unlockState();
  if (!connected) return 0;

  const uint32_t total = renderRequestCount();
  return total >= baseline ? total - baseline : 0;
}

bool CompanionBleService::notifyToggleMuteReleased(uint16_t* counter) {
  lockState();
  const bool ready = running_ && hostConnected_ && buttonEventCharacteristic_ && buttonEventSubscribed_;
  unlockState();
  if (!ready) {
    logPrintf("Companion BLE: mute event skipped\n");
    return false;
  }

  return publishButtonEvent(static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleMute),
                            static_cast<uint8_t>(CompanionProtocol::ButtonAction::Released), 0, counter);
}

bool CompanionBleService::notifyToggleHandReleased(uint16_t* counter) {
  lockState();
  const bool ready =
      running_ && hostConnected_ && buttonEventCharacteristic_ && buttonEventSubscribed_ && !hostStatus_.handLocked;
  unlockState();
  if (!ready) {
    logPrintf("Companion BLE: hand event skipped\n");
    return false;
  }

  return publishButtonEvent(static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleHand),
                            static_cast<uint8_t>(CompanionProtocol::ButtonAction::Released), 0, counter);
}

bool CompanionBleService::notifyToggleCameraReleased(uint16_t* counter) {
  lockState();
  const bool ready =
      running_ && hostConnected_ && buttonEventCharacteristic_ && buttonEventSubscribed_ && !hostStatus_.cameraLocked;
  unlockState();
  if (!ready) {
    logPrintf("Companion BLE: camera event skipped\n");
    return false;
  }

  return publishButtonEvent(static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleCamera),
                            static_cast<uint8_t>(CompanionProtocol::ButtonAction::Released), 0, counter);
}

bool CompanionBleService::notifyReactionReleased(CompanionProtocol::ButtonId reaction, uint16_t* counter) {
  if (!CompanionProtocol::isReactionButton(static_cast<uint8_t>(reaction))) return false;

  lockState();
  const bool ready = running_ && hostConnected_ && buttonEventCharacteristic_ && buttonEventSubscribed_;
  unlockState();
  if (!ready) {
    logPrintf("Companion BLE: reaction event skipped id=%u\n", static_cast<unsigned>(reaction));
    return false;
  }

  return publishButtonEvent(static_cast<uint8_t>(reaction),
                            static_cast<uint8_t>(CompanionProtocol::ButtonAction::Released), 0, counter);
}

bool CompanionBleService::notifySwitchDesktopReleased(uint8_t desktopIndex, uint16_t* counter) {
  lockState();
  const bool ready = running_ && hostConnected_ && buttonEventCharacteristic_ && buttonEventSubscribed_;
  const bool knownDesktop = desktopStatus_.valid && desktopIndex < desktopStatus_.count;
  const bool alreadyActive = knownDesktop && desktopStatus_.activeIndex == desktopIndex;
  unlockState();
  if (!ready || !knownDesktop) {
    logPrintf("Companion BLE: desktop switch skipped index=%u ready=%d known=%d\n",
              static_cast<unsigned>(desktopIndex), ready ? 1 : 0, knownDesktop ? 1 : 0);
    return false;
  }
  if (alreadyActive) {
    logPrintf("Companion BLE: desktop switch skipped; index=%u already active\n", static_cast<unsigned>(desktopIndex));
    return false;
  }

  return publishButtonEvent(static_cast<uint8_t>(CompanionProtocol::ButtonId::SwitchDesktop),
                            static_cast<uint8_t>(CompanionProtocol::ButtonAction::Released), desktopIndex, counter);
}

void CompanionBleService::onHostConnected(uint16_t connHandle) {
  lockState();
  resetSessionState();
  hostConnected_ = true;
  hostConnHandle_ = connHandle;
  hostConnectedAtMs_ = millis();
  bluetoothSessionRenderBaseline_ = renderRequestCount();
  activityStats_.gapConnects++;
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(callback);
  requestConnectionParams(ConnectionPowerProfile::Responsive, "connect");
}

void CompanionBleService::onHostDisconnected() {
  lockState();
  resetSessionState();
  publishHostStateValues();
  activityStats_.gapDisconnects++;
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(callback);
  restartAdvertising("disconnect");
}

void CompanionBleService::onConnParamsUpdated(uint16_t interval, uint16_t latency, uint16_t timeout) {
  setBtLockTraceConnectionParams(interval, latency);
  lockState();
  activityStats_.connParamUpdates++;
  negotiatedConnInterval_ = interval;
  negotiatedConnLatency_ = latency;
  negotiatedConnTimeout_ = timeout;
  hasNegotiatedConnParams_ = true;
  if (interval >= kConnIntervalIdleMin && interval <= kConnIntervalIdleMax && latency >= kConnLatencyIdle &&
      timeout >= kConnTimeoutIdle) {
    connectionProfile_ = ConnectionPowerProfile::Idle;
  } else if (interval <= kConnIntervalResponsiveMax && latency == kConnLatencyResponsive) {
    connectionProfile_ = ConnectionPowerProfile::Responsive;
  }
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(callback);
}

void CompanionBleService::onHostTeamsStateWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    unlockState();
    return;
  }

  bool next = false;
  uint16_t counter = 0;
  if (!decodeBoolState(value, &next, &counter)) {
    unlockState();
    return;
  }

  const bool changed = hostStatus_.teamsDetected != next || hostStatus_.teamsCounter != counter;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.teamsDetected = next;
  hostStatus_.teamsCounter = counter;
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("teams_state");
}

void CompanionBleService::onHostMeetingStateWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    unlockState();
    return;
  }

  bool next = false;
  uint16_t counter = 0;
  if (!decodeBoolState(value, &next, &counter)) {
    unlockState();
    return;
  }

  const bool changed = hostStatus_.meetingDetected != next || hostStatus_.meetingCounter != counter;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.meetingDetected = next;
  hostStatus_.meetingCounter = counter;
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("meeting_state");
}

void CompanionBleService::onHostMeetingNameWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  std::string next = characteristic->getValue();
  if (next.size() > kStatusMessageMaxLen) next.resize(kStatusMessageMaxLen);

  const bool changed = hostStatus_.meetingName != next;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.meetingName = next;
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("meeting_name");
}

void CompanionBleService::onHostMicrophoneStateWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    unlockState();
    return;
  }

  uint8_t next = 0;
  uint16_t counter = 0;
  if (!decodeTriState(value, &next, &counter)) {
    unlockState();
    return;
  }

  const bool acknowledged = pendingButtons_.mutePending && pendingButtons_.muteCounter == counter;
  const bool changed = hostStatus_.microphone != next || hostStatus_.microphoneCounter != counter || acknowledged;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.microphone = next;
  hostStatus_.microphoneCounter = counter;
  if (acknowledged) {
    const uint32_t latencyMs = millis() - pendingButtons_.mutePressedAtMs;
    pendingButtons_.mutePending = false;
    pendingButtons_.lastAcknowledgedValid = true;
    pendingButtons_.lastAcknowledgedButtonId = static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleMute);
    pendingButtons_.lastAcknowledgedCounter = counter;
    pendingButtons_.lastAcknowledgedLatencyMs = latencyMs;
    logPrintf("Companion BLE: mute button seq=%u acknowledged roundtripMs=%lu\n", static_cast<unsigned>(counter),
              static_cast<unsigned long>(latencyMs));
  }
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("microphone_state");
}

void CompanionBleService::onHostCameraStateWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    unlockState();
    return;
  }

  uint8_t next = 0;
  uint16_t counter = 0;
  bool locked = false;
  if (!decodeTriState(value, &next, &counter, &locked)) {
    unlockState();
    return;
  }

  const bool acknowledged = pendingButtons_.cameraPending && pendingButtons_.cameraCounter == counter;
  const bool changed =
      hostStatus_.camera != next || hostStatus_.cameraCounter != counter || hostStatus_.cameraLocked != locked ||
      acknowledged;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.camera = next;
  hostStatus_.cameraCounter = counter;
  hostStatus_.cameraLocked = locked;
  if (acknowledged) {
    const uint32_t latencyMs = millis() - pendingButtons_.cameraPressedAtMs;
    pendingButtons_.cameraPending = false;
    pendingButtons_.lastAcknowledgedValid = true;
    pendingButtons_.lastAcknowledgedButtonId = static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleCamera);
    pendingButtons_.lastAcknowledgedCounter = counter;
    pendingButtons_.lastAcknowledgedLatencyMs = latencyMs;
    logPrintf("Companion BLE: camera button seq=%u acknowledged roundtripMs=%lu\n", static_cast<unsigned>(counter),
              static_cast<unsigned long>(latencyMs));
  }
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("camera_state");
}

void CompanionBleService::onHostHandStateWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  const std::string value = characteristic->getValue();
  if (value.empty()) {
    unlockState();
    return;
  }

  uint8_t next = 0;
  uint16_t counter = 0;
  bool locked = false;
  if (!decodeTriState(value, &next, &counter, &locked)) {
    unlockState();
    return;
  }

  const bool acknowledged = pendingButtons_.handPending && pendingButtons_.handCounter == counter;
  const bool changed =
      hostStatus_.hand != next || hostStatus_.handCounter != counter || hostStatus_.handLocked != locked ||
      acknowledged;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.hand = next;
  hostStatus_.handCounter = counter;
  hostStatus_.handLocked = locked;
  if (acknowledged) {
    const uint32_t latencyMs = millis() - pendingButtons_.handPressedAtMs;
    pendingButtons_.handPending = false;
    pendingButtons_.lastAcknowledgedValid = true;
    pendingButtons_.lastAcknowledgedButtonId = static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleHand);
    pendingButtons_.lastAcknowledgedCounter = counter;
    pendingButtons_.lastAcknowledgedLatencyMs = latencyMs;
    logPrintf("Companion BLE: hand button seq=%u acknowledged roundtripMs=%lu\n", static_cast<unsigned>(counter),
              static_cast<unsigned long>(latencyMs));
  }
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("hand_state");
}

void CompanionBleService::onHostStatusMessageWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  std::string next = characteristic->getValue();
  if (next.size() > kStatusMessageMaxLen) next.resize(kStatusMessageMaxLen);

  const bool changed = hostStatus_.message != next;
  const bool firstStateWrite = !hostStateReceived_;
  hostStatus_.message = next;
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("message_state");
}

void CompanionBleService::onHostDesktopStateWritten(NimBLECharacteristic* characteristic) {
  lockState();
  StatusChangedCallback callback = nullptr;
  activityStats_.hostWrites++;
  if (!characteristic) {
    unlockState();
    return;
  }

  const std::string value = characteristic->getValue();
  DesktopStatus next;
  uint16_t ackSequence = CompanionProtocol::DESKTOP_ACK_NONE;
  if (!decodeDesktopState(value, &next, &ackSequence)) {
    unlockState();
    return;
  }

  // The host echoes the sequence of the switch it finished handling, so a request settles here
  // whether or not it actually landed on the requested desktop.
  const bool acknowledged = pendingButtons_.desktopPending &&
                            ackSequence != CompanionProtocol::DESKTOP_ACK_NONE &&
                            ackSequence == pendingButtons_.desktopCounter;
  const bool changed = !desktopStatusEquals(desktopStatus_, next) || acknowledged;
  const bool firstStateWrite = !hostStateReceived_;
  desktopStatus_ = next;
  if (acknowledged) {
    const uint32_t latencyMs = millis() - pendingButtons_.desktopPressedAtMs;
    const bool landed = next.activeIndex == pendingButtons_.desktopTargetIndex;
    pendingButtons_.desktopPending = false;
    pendingButtons_.lastAcknowledgedValid = true;
    pendingButtons_.lastAcknowledgedButtonId = static_cast<uint8_t>(CompanionProtocol::ButtonId::SwitchDesktop);
    pendingButtons_.lastAcknowledgedCounter = pendingButtons_.desktopCounter;
    pendingButtons_.lastAcknowledgedLatencyMs = latencyMs;
    logPrintf("Companion BLE: desktop switch seq=%u acknowledged landed=%d roundtripMs=%lu\n",
              static_cast<unsigned>(pendingButtons_.desktopCounter), landed ? 1 : 0,
              static_cast<unsigned long>(latencyMs));
  }
  hostStateReceived_ = true;
  if (changed || firstStateWrite) {
    activityStats_.hostStateChanges++;
    callback = markStatusChangedLocked();
  }
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("desktop_state");
}

void CompanionBleService::onButtonEventSubscribed(bool subscribed) {
  lockState();
  buttonEventSubscribed_ = subscribed;
  activityStats_.buttonSubscribes++;
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(callback);
  requestIdleConnectionParamsIfReady("subscribe");
}

void CompanionBleService::onParticipationSubscribed(bool subscribed) {
  lockState();
  participationSubscribed_ = subscribed;
  activityStats_.participationSubscribes++;
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(callback);
}

CompanionBleService::StatusChangedCallback CompanionBleService::settlePendingButtonsLocked(unsigned long now) {
  bool settled = false;
  if (pendingButtons_.mutePending &&
      now - pendingButtons_.mutePressedAtMs > CompanionProtocol::STATE_PENDING_TIMEOUT_MS) {
    pendingButtons_.mutePending = false;
    settled = true;
  }
  if (pendingButtons_.cameraPending &&
      now - pendingButtons_.cameraPressedAtMs > CompanionProtocol::STATE_PENDING_TIMEOUT_MS) {
    pendingButtons_.cameraPending = false;
    settled = true;
  }
  if (pendingButtons_.handPending &&
      now - pendingButtons_.handPressedAtMs > CompanionProtocol::STATE_PENDING_TIMEOUT_MS) {
    pendingButtons_.handPending = false;
    settled = true;
  }
  // Reactions have no state coming back, so they are held for a minimum time to stay
  // visible, then cleared once transmitted. The timeout is the fallback if the handover
  // never confirms.
  if (pendingButtons_.reactionPending) {
    const uint32_t heldMs = static_cast<uint32_t>(now - pendingButtons_.reactionPressedAtMs);
    const bool minimumShown = heldMs >= CompanionProtocol::REACTION_MIN_PRESSED_MS;
    const bool timedOut = heldMs > CompanionProtocol::STATE_PENDING_TIMEOUT_MS;
    if ((pendingButtons_.reactionTransmitted && minimumShown) || timedOut) {
      pendingButtons_.reactionPending = false;
      settled = true;
    }
  }
  return settled ? markStatusChangedLocked() : nullptr;
}

void CompanionBleService::update() {
  lockState();
  if (!running_) {
    unlockState();
    return;
  }

  activityStats_.updateCalls++;
  const bool shouldCheckParticipation = hostConnected_ && participationSubscribed_ && participationUntilMs_ != 0;
  // Pressed styling has to settle on its own schedule, so it is checked every tick rather
  // than on the much slower maintenance interval.
  const StatusChangedCallback settleCallback = settlePendingButtonsLocked(millis());
  unlockState();

  notifyStatusChanged(settleCallback);

  if (shouldCheckParticipation) {
    publishParticipationEventIfDue();
  }

  lockState();
  if (!running_) {
    unlockState();
    return;
  }

  const unsigned long now = millis();
  if (lastMaintenanceAtMs_ != 0 && now - lastMaintenanceAtMs_ < kMaintenanceIntervalMs) {
    unlockState();
    return;
  }

  lastMaintenanceAtMs_ = now;
  activityStats_.maintenanceRuns++;
  const bool connected = hostConnected_;
  const bool responsiveDue = responsiveUntilMs_ != 0 && static_cast<long>(now - responsiveUntilMs_) >= 0;
  // A host that never answers a switch request must not leave the page stuck on "pressed".
  bool desktopSwitchTimedOut = false;
  if (pendingButtons_.desktopPending &&
      now - pendingButtons_.desktopPressedAtMs > CompanionProtocol::DESKTOP_SWITCH_TIMEOUT_MS) {
    pendingButtons_.desktopPending = false;
    desktopSwitchTimedOut = true;
  }
  // A toggle whose new state is never echoed back must still settle; that is handled every
  // tick by settlePendingButtonsLocked().
  const bool hostSubscribed = buttonEventSubscribed_ || participationSubscribed_;
  const bool staleHandshake = hostConnectedAtMs_ != 0 && !hostStateReceived_ && !hostSubscribed &&
                              now - hostConnectedAtMs_ > kHandshakeTimeoutMs && server_;
  const uint16_t staleHandle = hostConnHandle_;
  const StatusChangedCallback timeoutCallback = desktopSwitchTimedOut ? markStatusChangedLocked() : nullptr;
  unlockState();

  if (desktopSwitchTimedOut) {
    logPrintf("Companion BLE: desktop switch request timed out\n");
    notifyStatusChanged(timeoutCallback);
  }

  if (!connected) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (!advertising || !advertising->isAdvertising()) {
      restartAdvertising("maintenance");
    }
    return;
  }

  if (responsiveDue) {
    lockState();
    responsiveUntilMs_ = 0;
    unlockState();
    requestIdleConnectionParamsIfReady("responsive_elapsed");
  }

  if (staleHandshake && server_) {
    logPrintf("Companion BLE: disconnecting stale host handshake\n");
    server_->disconnect(staleHandle);
  }
}

void CompanionBleService::resetSessionState() {
  lockState();
  hostConnected_ = false;
  hostStateReceived_ = false;
  buttonEventSubscribed_ = false;
  participationSubscribed_ = false;
  connectionProfile_ = ConnectionPowerProfile::Unknown;
  requestedConnectionProfile_ = ConnectionPowerProfile::Unknown;
  hostConnHandle_ = 0xFFFF;
  requestedConnIntervalMin_ = 0;
  requestedConnIntervalMax_ = 0;
  requestedConnLatency_ = 0;
  requestedConnTimeout_ = 0;
  negotiatedConnInterval_ = 0;
  negotiatedConnLatency_ = 0;
  negotiatedConnTimeout_ = 0;
  hostConnectedAtMs_ = 0;
  lastConnParamRequestAtMs_ = 0;
  responsiveUntilMs_ = 0;
  participationUntilMs_ = 0;
  lastParticipationNotifyAtMs_ = 0;
  hasNegotiatedConnParams_ = false;
  bluetoothSessionRenderBaseline_ = 0;
  hostStatus_ = HostStatus{};
  desktopStatus_ = DesktopStatus{};
  pendingButtons_ = PendingButtonStatus{};
  unlockState();
}

void CompanionBleService::requestConnectionParams(ConnectionPowerProfile profile, const char* reason) {
  lockState();
  if (!server_ || !hostConnected_ || hostConnHandle_ == 0xFFFF) {
    unlockState();
    return;
  }

  const unsigned long now = millis();
  if (connectionProfile_ == profile) {
    unlockState();
    return;
  }
  if (lastConnParamRequestAtMs_ != 0 && now - lastConnParamRequestAtMs_ < kConnParamRequestMinIntervalMs) {
    unlockState();
    return;
  }

  uint16_t minInterval = kConnIntervalIdleMin;
  uint16_t maxInterval = kConnIntervalIdleMax;
  uint16_t latency = kConnLatencyIdle;
  uint16_t timeout = kConnTimeoutIdle;
  if (profile == ConnectionPowerProfile::Responsive) {
    minInterval = kConnIntervalResponsiveMin;
    maxInterval = kConnIntervalResponsiveMax;
    latency = kConnLatencyResponsive;
    timeout = kConnTimeoutResponsive;
  }

  lastConnParamRequestAtMs_ = now;
  connectionProfile_ = profile;
  requestedConnectionProfile_ = profile;
  requestedConnIntervalMin_ = minInterval;
  requestedConnIntervalMax_ = maxInterval;
  requestedConnLatency_ = latency;
  requestedConnTimeout_ = timeout;
  activityStats_.connParamRequests++;
  server_->updateConnParams(hostConnHandle_, minInterval, maxInterval, latency, timeout);
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  logPrintf("Companion BLE: requested %s connection params (%s)\n", profileName(profile), reason ? reason : "");
  notifyStatusChanged(callback);
}

void CompanionBleService::requestIdleConnectionParamsIfReady(const char* reason) {
  lockState();
  const bool ready = hostConnected_ && hostStateReceived_ && buttonEventSubscribed_ && responsiveUntilMs_ == 0;
  unlockState();
  if (!ready) return;
  requestConnectionParams(ConnectionPowerProfile::Idle, reason);
}

bool CompanionBleService::restartAdvertising(const char* reason) {
  if (!running_) return false;

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (advertising && advertising->isAdvertising()) return true;

  const unsigned long now = millis();
  if (lastAdvertisingRestartAtMs_ != 0 && now - lastAdvertisingRestartAtMs_ < kAdvertisingRestartIntervalMs) {
    return false;
  }

  lastAdvertisingRestartAtMs_ = now;
  const bool ok = NimBLEDevice::startAdvertising();
  lockState();
  activityStats_.advertisingRestarts++;
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  logPrintf("Companion BLE: advertising restart %s ok=%d\n", reason ? reason : "", ok);
  notifyStatusChanged(callback);
  return ok;
}

void CompanionBleService::publishHostStateValues() {
  lockState();
  setEncodedStateValue(hostTeamsStateCharacteristic_, hostStatus_.teamsDetected, hostStatus_.teamsCounter);
  setEncodedStateValue(hostMeetingStateCharacteristic_, hostStatus_.meetingDetected, hostStatus_.meetingCounter);
  if (hostMeetingNameCharacteristic_) hostMeetingNameCharacteristic_->setValue(hostStatus_.meetingName);
  setEncodedStateValue(hostMicrophoneStateCharacteristic_,
                       hostStatus_.microphone == static_cast<uint8_t>(CompanionProtocol::TriState::On),
                       hostStatus_.microphoneCounter);
  setEncodedStateValue(hostCameraStateCharacteristic_,
                       hostStatus_.camera == static_cast<uint8_t>(CompanionProtocol::TriState::On),
                       hostStatus_.cameraCounter, hostStatus_.cameraLocked);
  setEncodedStateValue(hostHandStateCharacteristic_,
                       hostStatus_.hand == static_cast<uint8_t>(CompanionProtocol::TriState::On),
                       hostStatus_.handCounter, hostStatus_.handLocked);
  if (hostStatusMessageCharacteristic_) hostStatusMessageCharacteristic_->setValue(hostStatus_.message);
  if (hostDesktopStateCharacteristic_) hostDesktopStateCharacteristic_->setValue(encodeDesktopState(desktopStatus_));
  unlockState();
}

void CompanionBleService::publishDeviceInfo() {
  if (!deviceInfoCharacteristic_) return;

  const uint8_t payload[] = {
      CompanionProtocol::PROTOCOL_VERSION,
      0x03,
  };
  deviceInfoCharacteristic_->setValue(payload, sizeof(payload));
}

bool CompanionBleService::publishButtonEvent(uint8_t buttonId, uint8_t action, uint8_t argument, uint16_t* counter) {
  if (!buttonEventCharacteristic_) return false;

  lockState();
  buttonEventSequence_ = nextButtonCounter(buttonEventSequence_);
  const uint16_t sequence = buttonEventSequence_;
  const unsigned long now = millis();
  responsiveUntilMs_ = now + kButtonResponsiveWindowMs;
  participationUntilMs_ = now + kParticipationWindowMs;
  lastParticipationNotifyAtMs_ = 0;
  if (buttonId == static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleMute)) {
    pendingButtons_.mutePending = true;
    pendingButtons_.muteCounter = sequence;
    pendingButtons_.mutePressedAtMs = now;
  } else if (buttonId == static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleHand)) {
    pendingButtons_.handPending = true;
    pendingButtons_.handCounter = sequence;
    pendingButtons_.handPressedAtMs = now;
  } else if (buttonId == static_cast<uint8_t>(CompanionProtocol::ButtonId::ToggleCamera)) {
    pendingButtons_.cameraPending = true;
    pendingButtons_.cameraCounter = sequence;
    pendingButtons_.cameraPressedAtMs = now;
  } else if (buttonId == static_cast<uint8_t>(CompanionProtocol::ButtonId::SwitchDesktop)) {
    pendingButtons_.desktopPending = true;
    pendingButtons_.desktopCounter = sequence;
    pendingButtons_.desktopPressedAtMs = now;
    pendingButtons_.desktopTargetIndex = argument;
  } else if (CompanionProtocol::isReactionButton(buttonId)) {
    pendingButtons_.reactionPending = true;
    pendingButtons_.reactionTransmitted = false;
    pendingButtons_.reactionButtonId = buttonId;
    pendingButtons_.reactionPressedAtMs = now;
  }
  const StatusChangedCallback pendingCallback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(pendingCallback);
  requestConnectionParams(ConnectionPowerProfile::Responsive, "button_event");

  const uint32_t uptimeMs = millis();
  const uint8_t payload[] = {
      CompanionProtocol::PROTOCOL_VERSION,
      buttonId,
      action,
      static_cast<uint8_t>(sequence & 0xFF),
      static_cast<uint8_t>((sequence >> 8) & 0xFF),
      static_cast<uint8_t>(uptimeMs & 0xFF),
      static_cast<uint8_t>((uptimeMs >> 8) & 0xFF),
      static_cast<uint8_t>((uptimeMs >> 16) & 0xFF),
      static_cast<uint8_t>((uptimeMs >> 24) & 0xFF),
      argument,
  };
  static_assert(sizeof(payload) == CompanionProtocol::BUTTON_EVENT_PAYLOAD_LEN, "button event payload size mismatch");
  logPrintf("Companion BLE: publishing button event id=%u action=%u seq=%u arg=%u uptimeMs=%lu\n",
            static_cast<unsigned>(buttonId), static_cast<unsigned>(action), static_cast<unsigned>(sequence),
            static_cast<unsigned>(argument), static_cast<unsigned long>(uptimeMs));
  buttonEventCharacteristic_->setValue(payload, sizeof(payload));
  const bool notified = buttonEventCharacteristic_->notify();
  if (counter) {
    *counter = sequence;
  }
  lockState();
  activityStats_.buttonNotifications++;
  // A reaction has no state to come back, so the handover itself is the confirmation. The
  // pressed styling is not dropped here: settlePendingButtonsLocked() holds it briefly so it
  // does not vanish before the panel has drawn it.
  if (pendingButtons_.reactionPending && pendingButtons_.reactionButtonId == buttonId && notified) {
    pendingButtons_.reactionTransmitted = true;
  }
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  if (!notified) {
    logPrintf("Companion BLE: button notify failed id=%u seq=%u\n", static_cast<unsigned>(buttonId),
              static_cast<unsigned>(sequence));
  }
  notifyStatusChanged(callback);
  publishParticipationEventIfDue();
  return true;
}

void CompanionBleService::publishParticipationEventIfDue() {
  if (!participationCharacteristic_) return;

  uint32_t counter = 0;
  unsigned long now = 0;
  unsigned long untilMs = 0;
  unsigned long sinceLastMs = 0;
  unsigned long notifyPeriodMs = 0;
  uint16_t interval = 0;
  uint16_t negotiatedInterval = 0;
  uint16_t requestedMax = 0;
  uint16_t connHandle = 0xFFFF;
  lockState();
  now = millis();
  activityStats_.participationTimerChecks++;
  if (!hostConnected_ || !participationSubscribed_ || participationUntilMs_ == 0) {
    unlockState();
    logPrintf("Companion BLE: participation timer inactive connected=%d subscribed=%d untilMs=%lu nowMs=%lu\n",
              hostConnected_ ? 1 : 0, participationSubscribed_ ? 1 : 0,
              static_cast<unsigned long>(participationUntilMs_), static_cast<unsigned long>(now));
    return;
  }

  untilMs = participationUntilMs_;
  if (static_cast<long>(now - participationUntilMs_) >= 0) {
    participationUntilMs_ = 0;
    unlockState();
    logPrintf("Companion BLE: participation timer expired nowMs=%lu untilMs=%lu sent=%lu\n",
              static_cast<unsigned long>(now), static_cast<unsigned long>(untilMs),
              static_cast<unsigned long>(participationCounter_));
    return;
  }

  negotiatedInterval = negotiatedConnInterval_;
  requestedMax = requestedConnIntervalMax_;
  interval = negotiatedInterval != 0 ? negotiatedInterval : requestedMax;
  if (interval == 0) interval = kConnIntervalIdleMax;
  notifyPeriodMs = std::max(20UL, static_cast<unsigned long>(interval));
  sinceLastMs = lastParticipationNotifyAtMs_ == 0 ? 0 : now - lastParticipationNotifyAtMs_;
  if (lastParticipationNotifyAtMs_ != 0 && now - lastParticipationNotifyAtMs_ < notifyPeriodMs) {
    unlockState();
    logPrintf("Companion BLE: participation timer tick waiting nowMs=%lu untilMs=%lu sinceLastMs=%lu periodMs=%lu "
              "interval=%u negotiated=%u requestedMax=%u\n",
              static_cast<unsigned long>(now), static_cast<unsigned long>(untilMs),
              static_cast<unsigned long>(sinceLastMs), static_cast<unsigned long>(notifyPeriodMs),
              static_cast<unsigned>(interval), static_cast<unsigned>(negotiatedInterval),
              static_cast<unsigned>(requestedMax));
    return;
  }

  lastParticipationNotifyAtMs_ = now;
  counter = participationCounter_ + 1;
  connHandle = hostConnHandle_;
  activityStats_.participationNotificationAttempts++;
  unlockState();

  const uint8_t payload[] = {
      CompanionProtocol::PROTOCOL_VERSION,
      static_cast<uint8_t>(counter & 0xFF),
      static_cast<uint8_t>((counter >> 8) & 0xFF),
      static_cast<uint8_t>((counter >> 16) & 0xFF),
      static_cast<uint8_t>((counter >> 24) & 0xFF),
  };
  participationCharacteristic_->setValue(payload, sizeof(payload));
  const bool sent = participationCharacteristic_->notify(payload, sizeof(payload), connHandle);
  lockState();
  if (sent) {
    participationCounter_ = counter;
    activityStats_.participationNotifications++;
  } else {
    activityStats_.participationNotificationFailures++;
  }
  unlockState();
  logPrintf("Companion BLE: participation notify %s counter=%lu handle=%u nowMs=%lu untilMs=%lu sinceLastMs=%lu "
            "periodMs=%lu interval=%u negotiated=%u requestedMax=%u\n",
            sent ? "sent" : "failed", static_cast<unsigned long>(counter), static_cast<unsigned>(connHandle),
            static_cast<unsigned long>(now), static_cast<unsigned long>(untilMs),
            static_cast<unsigned long>(sinceLastMs), static_cast<unsigned long>(notifyPeriodMs),
            static_cast<unsigned>(interval), static_cast<unsigned>(negotiatedInterval),
            static_cast<unsigned>(requestedMax));
}

std::string CompanionBleService::formatTimingDiagnostics() const {
  lockState();
  const bool advertising = isAdvertising();
  const bool connected = hostConnected_;
  const bool negotiated = hasNegotiatedConnParams_;
  const uint16_t negotiatedInterval = negotiatedConnInterval_;
  const uint16_t negotiatedLatency = negotiatedConnLatency_;
  const uint16_t negotiatedTimeout = negotiatedConnTimeout_;
  const ConnectionPowerProfile requestedProfile = requestedConnectionProfile_;
  const uint16_t requestedMin = requestedConnIntervalMin_;
  const uint16_t requestedMax = requestedConnIntervalMax_;
  const uint16_t requestedLatency = requestedConnLatency_;
  unlockState();

  char advMin[12];
  char advMax[12];
  char prefMin[12];
  char prefMax[12];
  formatTenthsMs(advIntervalTenthsMs(kAdvIntervalLowPowerMin), advMin, sizeof(advMin));
  formatTenthsMs(advIntervalTenthsMs(kAdvIntervalLowPowerMax), advMax, sizeof(advMax));
  formatTenthsMs(connIntervalTenthsMs(kConnIntervalIdleMin), prefMin, sizeof(prefMin));
  formatTenthsMs(connIntervalTenthsMs(kConnIntervalIdleMax), prefMax, sizeof(prefMax));

  char line[176];
  snprintf(line, sizeof(line), "Adv %s-%s pref %s-%s %s", advMin, advMax, prefMin, prefMax,
           advertising ? "on" : "off");
  std::string result(line);
  result += "\n";

  char reqMin[12] = "?";
  char reqMax[12] = "?";
  if (requestedMin != 0 && requestedMax != 0) {
    formatTenthsMs(connIntervalTenthsMs(requestedMin), reqMin, sizeof(reqMin));
    formatTenthsMs(connIntervalTenthsMs(requestedMax), reqMax, sizeof(reqMax));
  }

  if (connected && negotiated) {
    char got[12];
    formatTenthsMs(connIntervalTenthsMs(negotiatedInterval), got, sizeof(got));
    snprintf(line, sizeof(line), "Conn got %s L%u T%us req %s %s-%s L%u", got,
             static_cast<unsigned>(negotiatedLatency), static_cast<unsigned>(negotiatedTimeout / 100U),
             profileName(requestedProfile), reqMin, reqMax, static_cast<unsigned>(requestedLatency));
  } else {
    snprintf(line, sizeof(line), "Conn %s req %s %s-%s L%u", connected ? "pending" : "disc",
             profileName(requestedProfile), reqMin, reqMax, static_cast<unsigned>(requestedLatency));
  }
  result += line;
  return result;
}

std::string CompanionBleService::formatActivityDeltaDiagnostics() {
  lockState();
  if (!hasPreviousActivityStats_) {
    previousActivityStats_ = activityStats_;
    hasPreviousActivityStats_ = true;
    unlockState();
    return "Activity baseline";
  }

  const ActivityStats current = activityStats_;
  const ActivityStats previous = previousActivityStats_;
  previousActivityStats_ = current;
  unlockState();
  char line[176];
  snprintf(line, sizeof(line), "upd+%lu m+%lu wr+%lu chg+%lu sub+%lu ntf+%lu tick+%lu part+%lu/%lu/%lu req+%lu got+%lu adv+%lu",
           static_cast<unsigned long>(deltaCounter(current.updateCalls, previous.updateCalls)),
           static_cast<unsigned long>(deltaCounter(current.maintenanceRuns, previous.maintenanceRuns)),
           static_cast<unsigned long>(deltaCounter(current.hostWrites, previous.hostWrites)),
           static_cast<unsigned long>(deltaCounter(current.hostStateChanges, previous.hostStateChanges)),
           static_cast<unsigned long>(deltaCounter(current.buttonSubscribes, previous.buttonSubscribes)),
           static_cast<unsigned long>(deltaCounter(current.buttonNotifications, previous.buttonNotifications)),
           static_cast<unsigned long>(deltaCounter(current.participationTimerChecks,
                                                   previous.participationTimerChecks)),
           static_cast<unsigned long>(deltaCounter(current.participationNotificationAttempts,
                                                   previous.participationNotificationAttempts)),
           static_cast<unsigned long>(
               deltaCounter(current.participationNotifications, previous.participationNotifications)),
           static_cast<unsigned long>(deltaCounter(current.participationNotificationFailures,
                                                   previous.participationNotificationFailures)),
           static_cast<unsigned long>(deltaCounter(current.connParamRequests, previous.connParamRequests)),
           static_cast<unsigned long>(deltaCounter(current.connParamUpdates, previous.connParamUpdates)),
           static_cast<unsigned long>(deltaCounter(current.advertisingRestarts, previous.advertisingRestarts)));
  return line;
}

CompanionBleService::StatusChangedCallback CompanionBleService::markStatusChangedLocked() {
  statusChanged_ = true;
  return statusChangedCallback_;
}

void CompanionBleService::markStatusChanged() {
  lockState();
  const StatusChangedCallback callback = markStatusChangedLocked();
  unlockState();
  notifyStatusChanged(callback);
}

void CompanionBleService::notifyStatusChanged(StatusChangedCallback callback) const {
  if (callback) {
    callback();
  }
}

bool CompanionBleService::ensureStateMutex() {
  if (stateMutex_) return true;
  stateMutex_ = xSemaphoreCreateRecursiveMutex();
  return stateMutex_ != nullptr;
}

void CompanionBleService::lockState() const {
  if (stateMutex_) {
    xSemaphoreTakeRecursive(stateMutex_, portMAX_DELAY);
  }
}

void CompanionBleService::unlockState() const {
  if (stateMutex_) {
    xSemaphoreGiveRecursive(stateMutex_);
  }
}

void CompanionBleService::startWorker() {
  if (workerTask_) return;
  workerStopRequested_ = false;
  xTaskCreate(workerTrampoline, "companion_ble", 4096, this, 2, &workerTask_);
}

void CompanionBleService::stopWorker() {
  if (!workerTask_) return;
  workerStopRequested_ = true;
  const TaskHandle_t task = workerTask_;
  workerTask_ = nullptr;
  vTaskDelete(task);
}

void CompanionBleService::workerTrampoline(void* self) {
  static_cast<CompanionBleService*>(self)->workerLoop();
}

void CompanionBleService::workerLoop() {
  while (!workerStopRequested_) {
    update();
    lockState();
    const bool participationActive = hostConnected_ && participationSubscribed_ && participationUntilMs_ != 0;
    unlockState();
    vTaskDelay(pdMS_TO_TICKS(participationActive ? kWorkerPollActiveMs : kWorkerPollMs));
  }
  vTaskDelete(nullptr);
}

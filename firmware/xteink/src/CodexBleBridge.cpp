#include "CodexBleBridge.h"

#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <inttypes.h>

static constexpr const char* CODEX_SERVICE_UUID = "6f30d210-2f6d-4a8c-9f78-42d8d2f04201";
static constexpr const char* CODEX_STATE_UUID = "6f30d211-2f6d-4a8c-9f78-42d8d2f04201";
static constexpr const char* CODEX_WRITE_UUID = "6f30d212-2f6d-4a8c-9f78-42d8d2f04201";
static constexpr const char* CODEX_EVENT_UUID = "6f30d213-2f6d-4a8c-9f78-42d8d2f04201";

static CodexBleBridge* activeBridge = nullptr;

class CodexBridgeServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    if (activeBridge) activeBridge->onConnect();
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    if (activeBridge) activeBridge->onDisconnect();
  }
};

class CodexBridgeWriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    if (!activeBridge) return;
    String value = characteristic->getValue();
    activeBridge->onWrite(std::string(value.c_str(), value.length()));
  }
};

static CodexBridgeServerCallbacks serverCallbacks;
static CodexBridgeWriteCallbacks writeCallbacks;

void CodexBleBridge::begin(CodexStatus* status) {
  status_ = status;
  activeBridge = this;

  if (!BLEDevice::init("Codex XTEINK")) {
    Serial.println("CODEX BLE INIT FAILED");
    return;
  }
  BLEDevice::setMTU(247);

  server_ = BLEDevice::createServer();
  server_->setCallbacks(&serverCallbacks);

  BLEService* service = server_->createService(CODEX_SERVICE_UUID);
  stateChar_ =
      service->createCharacteristic(CODEX_STATE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  writeChar_ = service->createCharacteristic(CODEX_WRITE_UUID,
                                             BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  eventChar_ =
      service->createCharacteristic(CODEX_EVENT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);

  writeChar_->setCallbacks(&writeCallbacks);
  stateChar_->setValue("ready");
  eventChar_->setValue("{\"type\":\"boot\"}");
  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setName("Codex XTEINK");
  advertising->addServiceUUID(CODEX_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
}

bool CodexBleBridge::poll() {
  if (!pendingWrite_ || !status_) return false;

  char payload[sizeof(pendingPayload_)];
  noInterrupts();
  strlcpy(payload, pendingPayload_, sizeof(payload));
  pendingWrite_ = false;
  interrupts();

  char result[64];
  const bool ok = status_->applyJson(payload, result, sizeof(result));
  if (ok) {
    ++acceptedCount_;
    char state[160];
    snprintf(state, sizeof(state), "{\"type\":\"state\",\"rev\":%" PRIu32 ",\"accepted\":%" PRIu32 "}",
             status_->revision, acceptedCount_);
    if (stateChar_) stateChar_->setValue(state);
    notifyJson(state);
  } else {
    ++rejectedCount_;
    char err[160];
    snprintf(err, sizeof(err), "{\"type\":\"error\",\"message\":\"%s\",\"rejected\":%" PRIu32 "}", result,
             rejectedCount_);
    notifyJson(err);
  }
  return ok;
}

void CodexBleBridge::notifyButton(const char* button, uint8_t page, uint8_t theme) {
  char event[160];
  snprintf(event, sizeof(event), "{\"type\":\"button\",\"button\":\"%s\",\"page\":%u,\"theme\":%u}", button, page,
           theme);
  notifyJson(event);
}

void CodexBleBridge::onConnect() {
  connected_ = true;
  notifyJson("{\"type\":\"connected\"}");
}

void CodexBleBridge::onDisconnect() {
  connected_ = false;
  BLEDevice::startAdvertising();
}

void CodexBleBridge::onWrite(const std::string& value) {
  const size_t n = min(value.size(), sizeof(pendingPayload_) - 1);
  memcpy(pendingPayload_, value.data(), n);
  pendingPayload_[n] = '\0';
  pendingWrite_ = true;
}

void CodexBleBridge::notifyJson(const char* json) {
  if (!eventChar_ || !json) return;
  eventChar_->setValue(json);
  if (connected_) eventChar_->notify();
}

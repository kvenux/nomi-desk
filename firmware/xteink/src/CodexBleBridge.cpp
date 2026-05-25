#include "CodexBleBridge.h"

#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <inttypes.h>

static constexpr const char* NOMI_DEVICE_NAME = "Nomi XTEINK";
static constexpr const char* NOMI_SERVICE_UUID = "f4f688c2-613e-56a5-b115-d19a99d1b463";
static constexpr const char* NOMI_RX_UUID = "74879a99-7275-5b33-9665-51519f328fa5";
static constexpr const char* NOMI_TX_UUID = "830ac719-8dea-541c-8d18-5e8de4cd83dd";
static constexpr const char* NOMI_INFO_UUID = "485d9275-a3ad-516d-a524-e284f0aafdb1";

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

  if (!BLEDevice::init(NOMI_DEVICE_NAME)) {
    Serial.println("NOMI XTEINK BLE INIT FAILED");
    return;
  }
  BLEDevice::setMTU(247);

  server_ = BLEDevice::createServer();
  server_->setCallbacks(&serverCallbacks);

  BLEService* service = server_->createService(NOMI_SERVICE_UUID);
  writeChar_ = service->createCharacteristic(NOMI_RX_UUID,
                                             BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  eventChar_ =
      service->createCharacteristic(NOMI_TX_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  infoChar_ = service->createCharacteristic(NOMI_INFO_UUID, BLECharacteristic::PROPERTY_READ);

  writeChar_->setCallbacks(&writeCallbacks);
  eventChar_->setValue("{\"xteink\":true}");
  infoChar_->setValue("{\"protocol\":\"nomi-agent-display\",\"version\":1,\"device\":\"xteink\",\"width\":480,\"height\":800}");
  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setName(NOMI_DEVICE_NAME);
  advertising->addServiceUUID(NOMI_SERVICE_UUID);
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
    snprintf(state, sizeof(state), "{\"ack\":true,\"rev\":%" PRIu32 ",\"accepted\":%" PRIu32 "}",
             status_->revision, acceptedCount_);
    notifyJson(state);
  } else {
    ++rejectedCount_;
    char err[160];
    snprintf(err, sizeof(err), "{\"err\":true,\"message\":\"%s\",\"rejected\":%" PRIu32 "}", result,
             rejectedCount_);
    notifyJson(err);
  }
  return ok;
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

#pragma once

#include <Arduino.h>

#include <string>

#include "CodexStatus.h"

class BLECharacteristic;
class BLEServer;

class CodexBleBridge {
 public:
  void begin(CodexStatus* status);
  bool poll();
  bool isConnected() const { return connected_; }
  uint32_t acceptedCount() const { return acceptedCount_; }
  uint32_t rejectedCount() const { return rejectedCount_; }
  void notifyButton(const char* button, uint8_t page, uint8_t theme);

 private:
  friend class CodexBridgeServerCallbacks;
  friend class CodexBridgeWriteCallbacks;

  void onConnect();
  void onDisconnect();
  void onWrite(const std::string& value);
  void notifyJson(const char* json);

  CodexStatus* status_ = nullptr;
  BLEServer* server_ = nullptr;
  BLECharacteristic* stateChar_ = nullptr;
  BLECharacteristic* writeChar_ = nullptr;
  BLECharacteristic* eventChar_ = nullptr;
  volatile bool connected_ = false;
  volatile bool pendingWrite_ = false;
  char pendingPayload_[640] = {};
  uint32_t acceptedCount_ = 0;
  uint32_t rejectedCount_ = 0;
};

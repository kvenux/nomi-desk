#pragma once

#include <Arduino.h>

struct CodexStatus {
  char modelLine[48];
  char contextLine[32];
  char tokenLine[40];
  char project[48];
  char branch[32];
  char runState[24];
  char goalLine[48];
  uint8_t contextPct;
  uint8_t fiveHourPct;
  uint8_t weeklyPct;
  uint8_t tasksDone;
  uint8_t tasksTotal;
  uint32_t revision;
  unsigned long updatedAtMs;

  void setDefaults();
  bool applyJson(const char* payload, char* errorOut = nullptr, size_t errorLen = 0);
  void metaLine(char* out, size_t outLen) const;
  void tasksLine(char* out, size_t outLen) const;
};

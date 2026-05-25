#include "CodexStatus.h"

#include <ArduinoJson.h>
#include <ctype.h>

static uint8_t clampPct(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return static_cast<uint8_t>(value);
}

static void copyText(char* dst, size_t dstLen, const char* src) {
  if (!dst || dstLen == 0) return;
  if (!src) src = "";
  strlcpy(dst, src, dstLen);
}

static bool assignString(JsonVariantConst v, char* dst, size_t dstLen) {
  if (!v.is<const char*>()) return false;
  copyText(dst, dstLen, v.as<const char*>());
  return true;
}

static bool assignString(JsonObjectConst obj, const char* key, char* dst, size_t dstLen) {
  if (!obj[key].is<const char*>()) return false;
  copyText(dst, dstLen, obj[key].as<const char*>());
  return true;
}

static bool assignPct(JsonObjectConst obj, const char* key, uint8_t& out) {
  if (!obj[key].is<int>()) return false;
  out = clampPct(obj[key].as<int>());
  return true;
}

static bool assignPctFromString(JsonObjectConst obj, const char* key, uint8_t& out) {
  if (!obj[key].is<const char*>()) return false;
  const char* s = obj[key].as<const char*>();
  int lastValue = -1;
  while (*s) {
    if (isdigit(static_cast<unsigned char>(*s))) {
      lastValue = atoi(s);
      while (*s && isdigit(static_cast<unsigned char>(*s))) ++s;
      continue;
    }
    ++s;
  }
  if (lastValue < 0) return false;
  out = clampPct(lastValue);
  return true;
}

void CodexStatus::setDefaults() {
  copyText(modelLine, sizeof(modelLine), "gpt-5.5 high fast");
  copyText(project, sizeof(project), "MatrixSpec");
  copyText(branch, sizeof(branch), "main");
  copyText(runState, sizeof(runState), "Ready");
  copyText(contextLine, sizeof(contextLine), "79% used");
  copyText(tokenLine, sizeof(tokenLine), "1.46M total used");
  copyText(goalLine, sizeof(goalLine), "Goal achieved (53m)");
  contextPct = 79;
  fiveHourPct = 97;
  weeklyPct = 93;
  tasksDone = 4;
  tasksTotal = 4;
  revision = 0;
  updatedAtMs = millis();
}

bool CodexStatus::applyJson(const char* payload, char* errorOut, size_t errorLen) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    if (errorOut && errorLen) snprintf(errorOut, errorLen, "json:%s", err.c_str());
    return false;
  }

  JsonObjectConst obj = doc.as<JsonObjectConst>();
  if (obj.isNull()) {
    if (errorOut && errorLen) copyText(errorOut, errorLen, "json:object required");
    return false;
  }

  assignString(obj, "model_line", modelLine, sizeof(modelLine));
  assignString(obj, "modelLine", modelLine, sizeof(modelLine));
  assignString(obj, "model-with-reasoning", modelLine, sizeof(modelLine));
  assignString(obj, "project", project, sizeof(project));
  assignString(obj, "project-name", project, sizeof(project));
  assignString(obj, "branch", branch, sizeof(branch));
  assignString(obj, "git-branch", branch, sizeof(branch));
  assignString(obj, "run_state", runState, sizeof(runState));
  assignString(obj, "runState", runState, sizeof(runState));
  assignString(obj, "run-state", runState, sizeof(runState));
  assignString(obj, "context_text", contextLine, sizeof(contextLine));
  assignString(obj, "contextText", contextLine, sizeof(contextLine));
  assignString(obj, "context-used", contextLine, sizeof(contextLine));
  assignString(obj, "token_text", tokenLine, sizeof(tokenLine));
  assignString(obj, "tokenText", tokenLine, sizeof(tokenLine));
  assignString(obj, "used-tokens", tokenLine, sizeof(tokenLine));
  assignString(obj, "goal_text", goalLine, sizeof(goalLine));
  assignString(obj, "goalText", goalLine, sizeof(goalLine));

  assignPct(obj, "context_pct", contextPct);
  assignPct(obj, "contextPct", contextPct);
  assignPct(obj, "contextUsedPct", contextPct);
  assignPctFromString(obj, "context-used", contextPct);
  assignPct(obj, "five_hour_pct", fiveHourPct);
  assignPct(obj, "fiveHourPct", fiveHourPct);
  assignPct(obj, "five-hour-limit-pct", fiveHourPct);
  assignPctFromString(obj, "five-hour-limit", fiveHourPct);
  assignPct(obj, "weekly_pct", weeklyPct);
  assignPct(obj, "weeklyPct", weeklyPct);
  assignPct(obj, "weekly-limit-pct", weeklyPct);
  assignPctFromString(obj, "weekly-limit", weeklyPct);

  if (obj["tasks_done"].is<int>()) tasksDone = static_cast<uint8_t>(max(0, obj["tasks_done"].as<int>()));
  if (obj["tasksDone"].is<int>()) tasksDone = static_cast<uint8_t>(max(0, obj["tasksDone"].as<int>()));
  if (obj["tasks_total"].is<int>()) tasksTotal = static_cast<uint8_t>(max(0, obj["tasks_total"].as<int>()));
  if (obj["tasksTotal"].is<int>()) tasksTotal = static_cast<uint8_t>(max(0, obj["tasksTotal"].as<int>()));
  if (obj["task-progress"].is<const char*>()) {
    int done = 0;
    int total = 0;
    if (sscanf(obj["task-progress"].as<const char*>(), "%d/%d", &done, &total) == 2) {
      tasksDone = static_cast<uint8_t>(max(0, done));
      tasksTotal = static_cast<uint8_t>(max(0, total));
    }
  }

  JsonObjectConst statusLine = obj["status_line"].as<JsonObjectConst>();
  if (!statusLine.isNull()) {
    assignString(statusLine, "model-with-reasoning", modelLine, sizeof(modelLine));
    assignString(statusLine, "project-name", project, sizeof(project));
    assignString(statusLine, "git-branch", branch, sizeof(branch));
    assignString(statusLine, "run-state", runState, sizeof(runState));
    assignString(statusLine, "context-used", contextLine, sizeof(contextLine));
    assignString(statusLine, "used-tokens", tokenLine, sizeof(tokenLine));
    assignPctFromString(statusLine, "context-used", contextPct);
    assignPctFromString(statusLine, "five-hour-limit", fiveHourPct);
    assignPctFromString(statusLine, "weekly-limit", weeklyPct);
    if (statusLine["task-progress"].is<const char*>()) {
      int done = 0;
      int total = 0;
      if (sscanf(statusLine["task-progress"].as<const char*>(), "%d/%d", &done, &total) == 2) {
        tasksDone = static_cast<uint8_t>(max(0, done));
        tasksTotal = static_cast<uint8_t>(max(0, total));
      }
    }
  }

  ++revision;
  updatedAtMs = millis();
  if (errorOut && errorLen) copyText(errorOut, errorLen, "ok");
  return true;
}

void CodexStatus::metaLine(char* out, size_t outLen) const {
  snprintf(out, outLen, "%s · %s · %s", project, branch, runState);
}

void CodexStatus::tasksLine(char* out, size_t outLen) const {
  snprintf(out, outLen, "%u/%u", tasksDone, tasksTotal);
}

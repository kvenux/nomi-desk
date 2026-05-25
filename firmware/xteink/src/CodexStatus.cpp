#include "CodexStatus.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <string.h>

static constexpr const char* NOMI_PROTOCOL = "nomi-agent-display";
static constexpr int NOMI_VERSION = 1;

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

static void appendTextPart(char* dst, size_t dstLen, const char* part) {
  if (!dst || dstLen == 0 || !part || part[0] == '\0') return;

  size_t used = strlen(dst);
  if (used >= dstLen - 1) return;

  if (used > 0) {
    dst[used++] = ' ';
    dst[used] = '\0';
  }

  if (used < dstLen) {
    strlcpy(dst + used, part, dstLen - used);
  }
}

static bool ApplyNomiModelLine(JsonObjectConst obj, char* dst, size_t dstLen) {
  const bool hasModel = obj["model"].is<const char*>();
  const bool hasEffort = obj["effort"].is<const char*>();
  const bool hasTier = obj["tier"].is<const char*>();
  if (!hasModel && !hasEffort && !hasTier) return false;

  char line[48] = "";
  appendTextPart(line, sizeof(line), hasModel ? obj["model"].as<const char*>() : dst);
  if (hasEffort) appendTextPart(line, sizeof(line), obj["effort"].as<const char*>());
  if (hasTier) appendTextPart(line, sizeof(line), obj["tier"].as<const char*>());
  copyText(dst, dstLen, line);
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
  copyText(modelLine, sizeof(modelLine), "waiting for Nomi");
  copyText(project, sizeof(project), "codex");
  copyText(branch, sizeof(branch), "current session");
  copyText(runState, sizeof(runState), "offline");
  copyText(contextLine, sizeof(contextLine), "0% used");
  copyText(tokenLine, sizeof(tokenLine), "0K tokens used");
  copyText(goalLine, sizeof(goalLine), "waiting for payload");
  contextPct = 0;
  fiveHourPct = 0;
  weeklyPct = 0;
  tasksDone = 0;
  tasksTotal = 0;
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

  if (obj["protocol"].is<const char*>()) {
    const char* protocol = obj["protocol"].as<const char*>();
    if (strcmp(protocol, NOMI_PROTOCOL) != 0) {
      if (errorOut && errorLen) snprintf(errorOut, errorLen, "protocol:%s", protocol);
      return false;
    }
    if (!obj["version"].is<int>() || obj["version"].as<int>() != NOMI_VERSION) {
      if (errorOut && errorLen) snprintf(errorOut, errorLen, "version:%d", obj["version"] | -1);
      return false;
    }
  }

  assignString(obj, "source", project, sizeof(project));
  assignString(obj, "session", branch, sizeof(branch));
  assignString(obj, "state", runState, sizeof(runState));
  ApplyNomiModelLine(obj, modelLine, sizeof(modelLine));
  assignString(obj, "event", goalLine, sizeof(goalLine));
  assignString(obj, "prompt", goalLine, sizeof(goalLine));

  if (obj["context_pct"].is<int>()) {
    contextPct = clampPct(obj["context_pct"].as<int>());
    snprintf(contextLine, sizeof(contextLine), "%u%% used", static_cast<unsigned>(contextPct));
  }
  if (obj["used_tokens_k"].is<int>()) {
    const int tokens = max(0, obj["used_tokens_k"].as<int>());
    snprintf(tokenLine, sizeof(tokenLine), "%dK tokens used", tokens);
  }
  JsonObjectConst quota = obj["quota"].as<JsonObjectConst>();
  if (!quota.isNull()) {
    if (quota["five_hour_left"].is<int>()) fiveHourPct = clampPct(quota["five_hour_left"].as<int>());
    if (quota["weekly_left"].is<int>()) weeklyPct = clampPct(quota["weekly_left"].as<int>());
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
  snprintf(out, outLen, "%s / %s / %s", project, branch, runState);
}

void CodexStatus::tasksLine(char* out, size_t outLen) const {
  snprintf(out, outLen, "%u/%u", tasksDone, tasksTotal);
}

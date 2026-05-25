#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <InputManager.h>
#include <builtinFonts/all.h>
#include <driver/gpio.h>
#include <inttypes.h>

#include "CodexBleBridge.h"
#include "CodexStatus.h"
#include "fontIds.h"

static constexpr gpio_num_t POWER_LATCH_PIN = GPIO_NUM_13;

GfxRenderer renderer(display);
InputManager input;
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFontFamily notosans12Font(&notosans12RegularFont, &notosans12BoldFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFontFamily notosans14Font(&notosans14RegularFont, &notosans14BoldFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFontFamily notosans16Font(&notosans16RegularFont, &notosans16BoldFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFontFamily notosans18Font(&notosans18RegularFont, &notosans18BoldFont);
EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10Font(&ui10RegularFont, &ui10BoldFont);
EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12Font(&ui12RegularFont, &ui12BoldFont);

enum class Page : uint8_t { Overview, Metrics, Work, Themes, Count };
enum class Theme : uint8_t { Paper, Studio, Grid, Focus, Count };

static Page page = Page::Overview;
static Theme theme = Theme::Paper;
static CodexStatus status;
static CodexBleBridge bridge;
static char feedback[24] = "READY";
static unsigned long feedbackUntil = 0;
static uint32_t paintCount = 0;
static bool firstPaint = true;
static char serialLine[640];
static uint16_t serialLineLen = 0;

static void holdPower() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(POWER_LATCH_PIN);
  gpio_set_direction(POWER_LATCH_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(POWER_LATCH_PIN, 1);
}

static const char* pageTitle() {
  switch (page) {
    case Page::Overview:
      return "Overview";
    case Page::Metrics:
      return "Metrics";
    case Page::Work:
      return "Workflow";
    case Page::Themes:
      return "Themes";
    default:
      return "Nomi";
  }
}

static const char* themeTitle() {
  switch (theme) {
    case Theme::Paper:
      return "Paper";
    case Theme::Studio:
      return "Studio";
    case Theme::Grid:
      return "Grid";
    case Theme::Focus:
      return "Focus";
    default:
      return "Theme";
  }
}

static void setFeedback(const char* value) {
  strlcpy(feedback, value, sizeof(feedback));
  feedbackUntil = millis() + 2400;
}

static void text(int font, int x, int y, const char* value, bool black = true,
                 EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  renderer.drawText(font, x, y, value, black, style);
}

static void clippedText(int font, int x, int y, int maxWidth, const char* value, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  std::string clipped = renderer.truncatedText(font, value, maxWidth, style);
  renderer.drawText(font, x, y, clipped.c_str(), black, style);
}

static void centeredText(int font, int x, int y, int w, const char* value, bool black = true,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  int tw = renderer.getTextWidth(font, value, style);
  renderer.drawText(font, x + (w - tw) / 2, y, value, black, style);
}

static void registerFonts() {
  fontDecompressor.init();
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12Font);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14Font);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16Font);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18Font);
  renderer.insertFont(UI_10_FONT_ID, ui10Font);
  renderer.insertFont(UI_12_FONT_ID, ui12Font);
}

static void hairlineBox(int x, int y, int w, int h) {
  renderer.drawRoundedRect(x, y, w, h, 1, 8, true);
}

static void softPanel(int x, int y, int w, int h, const char* label) {
  renderer.fillRoundedRect(x, y, w, h, 8, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 1, 8, true);
  renderer.fillRectDither(x + 1, y + 1, w - 2, 32, Color::LightGray);
  text(UI_10_FONT_ID, x + 14, y + 21, label, true, EpdFontFamily::BOLD);
}

static void pill(int x, int y, int w, const char* value, bool selected = false) {
  if (selected) {
    renderer.fillRoundedRect(x, y, w, 26, 13, Color::Black);
    renderer.drawText(UI_10_FONT_ID, x + 12, y + 18, value, false, EpdFontFamily::BOLD);
  } else {
    renderer.drawRoundedRect(x, y, w, 26, 1, 13, true);
    renderer.drawText(UI_10_FONT_ID, x + 12, y + 18, value, true, EpdFontFamily::BOLD);
  }
}

static void progress(int x, int y, int w, int pct, const char* label) {
  text(UI_10_FONT_ID, x, y - 8, label, true, EpdFontFamily::BOLD);
  renderer.drawRoundedRect(x, y, w, 22, 1, 8, true);
  renderer.fillRoundedRect(x + 4, y + 4, (w - 8) * pct / 100, 14, 6, Color::Black);
  char value[12];
  snprintf(value, sizeof(value), "%d%%", pct);
  text(UI_10_FONT_ID, x + w - 42, y - 8, value, true, EpdFontFamily::BOLD);
}

static void divider(int y) {
  renderer.drawLine(48, y, 432, y, 2, true);
}

static void statusProgress(int x, int y, int w, int pct, bool hatch = false) {
  renderer.drawRoundedRect(x, y, w, 20, 1, 7, true);
  int fillW = (w - 6) * pct / 100;
  renderer.fillRoundedRect(x + 3, y + 3, fillW, 14, 5, Color::Black);
  if (hatch) {
    for (int xx = x + 3; xx < x + 3 + fillW + 18; xx += 12) {
      renderer.drawLine(xx - 14, y + 17, xx, y + 3, 2, false);
    }
  }
}

static void smallCubeIcon(int x, int y) {
  renderer.drawRect(x + 8, y, 18, 18, 1, true);
  renderer.drawLine(x + 8, y, x + 17, y - 7, 1, true);
  renderer.drawLine(x + 26, y, x + 35, y - 7, 1, true);
  renderer.drawLine(x + 17, y - 7, x + 35, y - 7, 1, true);
  renderer.drawLine(x + 26, y + 18, x + 35, y + 11, 1, true);
  renderer.drawLine(x + 35, y - 7, x + 35, y + 11, 1, true);
  renderer.drawLine(x + 17, y + 8, x + 35, y - 7, 1, true);
}

static void windowIcon(int x, int y) {
  renderer.drawRoundedRect(x, y, 30, 30, 3, 8, true);
  renderer.drawRect(x + 8, y + 8, 15, 15, 2, true);
}

static void clockIcon(int x, int y) {
  renderer.drawRoundedRect(x, y, 30, 30, 3, 15, true);
  renderer.drawLine(x + 15, y + 8, x + 15, y + 16, 2, true);
  renderer.drawLine(x + 15, y + 16, x + 22, y + 20, 2, true);
}

static void tasksIcon(int x, int y) {
  for (int i = 0; i < 3; ++i) {
    int yy = y + i * 10;
    renderer.drawLine(x, yy, x + 5, yy + 4, 2, true);
    renderer.drawLine(x + 5, yy + 4, x + 10, yy - 4, 2, true);
    renderer.drawLine(x + 18, yy, x + 38, yy, 2, true);
  }
}

static void tokenIcon(int x, int y) {
  renderer.drawRect(x + 3, y + 5, 24, 20, 3, true);
  renderer.drawLine(x + 9, y + 11, x + 21, y + 11, 2, true);
  renderer.drawLine(x + 9, y + 17, x + 21, y + 17, 2, true);
}

static void checkBadge(int x, int y) {
  renderer.drawRoundedRect(x, y, 48, 48, 3, 24, true);
  renderer.drawLine(x + 12, y + 26, x + 21, y + 35, 3, true);
  renderer.drawLine(x + 21, y + 35, x + 37, y + 15, 3, true);
}

static void background() {
  renderer.clearScreen(0xFF);
  if (theme == Theme::Grid) {
    for (int x = 0; x < 480; x += 24) renderer.drawLine(x, 0, x, 800, x % 48 == 0);
    for (int y = 0; y < 800; y += 24) renderer.drawLine(0, y, 480, y, y % 48 == 0);
  }
  if (theme == Theme::Studio) {
    renderer.fillRectDither(0, 0, 480, 128, Color::LightGray);
    renderer.fillRectDither(0, 694, 480, 106, Color::LightGray);
  }
  hairlineBox(14, 14, 452, 772);
}

static void header() {
  renderer.fillRoundedRect(28, 28, 424, 86, 10, Color::Black);
  text(UI_12_FONT_ID, 48, 56, "NOMI", false, EpdFontFamily::BOLD);
  text(NOTOSANS_18_FONT_ID, 48, 91, "XTEINK", false, EpdFontFamily::BOLD);
  text(UI_10_FONT_ID, 304, 56, pageTitle(), false, EpdFontFamily::BOLD);
  text(UI_10_FONT_ID, 304, 82, themeTitle(), false, EpdFontFamily::BOLD);

  pill(30, 132, 98, "LOCAL", true);
  pill(140, 132, 84, "APP1");
  if (millis() < feedbackUntil) pill(334, 132, 112, feedback, true);
}

static void footer() {
  renderer.drawLine(32, 728, 448, 728, 1, true);
  text(UI_10_FONT_ID, 44, 756, "LEFT RIGHT PAGE", true, EpdFontFamily::BOLD);
  text(UI_10_FONT_ID, 272, 756, "UP DOWN THEME", true, EpdFontFamily::BOLD);
}

static void overviewPage() {
  renderer.clearScreen(0xFF);
  renderer.drawRoundedRect(18, 18, 444, 764, 2, 8, true);
  renderer.drawRoundedRect(25, 25, 430, 750, 1, 5, true);

  text(NOTOSANS_18_FONT_ID, 58, 58, "Nomi Status", true, EpdFontFamily::BOLD);
  smallCubeIcon(394, 62);
  clippedText(UI_12_FONT_ID, 60, 112, 318, status.modelLine, true, EpdFontFamily::BOLD);
  char meta[128];
  status.metaLine(meta, sizeof(meta));
  clippedText(UI_12_FONT_ID, 60, 138, 318, meta, true);
  text(UI_10_FONT_ID, 382, 138, bridge.isConnected() ? "BLE" : "BLE-", true, EpdFontFamily::BOLD);
  divider(168);

  windowIcon(58, 192);
  text(NOTOSANS_14_FONT_ID, 104, 192, "Context window", true, EpdFontFamily::BOLD);
  clippedText(NOTOSANS_14_FONT_ID, 58, 240, 364, status.contextLine, true, EpdFontFamily::BOLD);
  statusProgress(58, 274, 364, status.contextPct, true);
  divider(318);

  tokenIcon(58, 342);
  text(NOTOSANS_14_FONT_ID, 104, 342, "Token usage", true, EpdFontFamily::BOLD);
  clippedText(NOTOSANS_14_FONT_ID, 58, 390, 364, status.tokenLine, true, EpdFontFamily::BOLD);
  divider(430);

  clockIcon(58, 454);
  text(NOTOSANS_14_FONT_ID, 104, 454, "Quota remaining", true, EpdFontFamily::BOLD);
  text(NOTOSANS_12_FONT_ID, 58, 502, "5h", true, EpdFontFamily::BOLD);
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", status.fiveHourPct);
  text(NOTOSANS_12_FONT_ID, 106, 502, pct, true);
  statusProgress(58, 536, 364, status.fiveHourPct);
  text(NOTOSANS_12_FONT_ID, 58, 584, "weekly", true, EpdFontFamily::BOLD);
  snprintf(pct, sizeof(pct), "%u%%", status.weeklyPct);
  text(NOTOSANS_12_FONT_ID, 156, 584, pct, true);
  statusProgress(58, 618, 364, status.weeklyPct, true);
  divider(662);

  tasksIcon(58, 686);
  text(NOTOSANS_14_FONT_ID, 104, 686, "Tasks", true, EpdFontFamily::BOLD);
  char tasks[16];
  status.tasksLine(tasks, sizeof(tasks));
  clippedText(NOTOSANS_16_FONT_ID, 58, 734, 54, tasks, true, EpdFontFamily::BOLD);
  clippedText(NOTOSANS_12_FONT_ID, 116, 742, 236, status.goalLine, true);
  checkBadge(366, 694);
}

static void metricsPage() {
  softPanel(30, 184, 420, 492, "RESOURCE STACK");
  progress(64, 260, 342, 68, "Context Budget");
  progress(64, 340, 342, 42, "5H Window");
  progress(64, 420, 342, 78, "Weekly Pool");
  renderer.drawRoundedRect(118, 502, 244, 100, 2, 14, true);
  renderer.fillRectDither(132, 516, 216, 72, Color::LightGray);
  text(NOTOSANS_18_FONT_ID, 178, 566, "68%", true, EpdFontFamily::BOLD);
  text(UI_12_FONT_ID, 174, 598, "USAGE RADAR", true, EpdFontFamily::BOLD);
}

static void workflowPage() {
  softPanel(30, 184, 420, 492, "WORKFLOW");
  const char* steps[] = {"READ", "PLAN", "PATCH", "FLASH", "VERIFY"};
  for (int i = 0; i < 5; ++i) {
    int y = 244 + i * 72;
    bool current = i == 4;
    if (current) renderer.fillRoundedRect(60, y, 92, 46, 8, Color::Black);
    else renderer.drawRoundedRect(60, y, 92, 46, 1, 8, true);
    centeredText(UI_10_FONT_ID, 60, y + 30, 92, steps[i], !current, EpdFontFamily::BOLD);
    text(NOTOSANS_14_FONT_ID, 182, y + 30, current ? "Screen proof" : "Done", true, EpdFontFamily::BOLD);
    if (i < 4) renderer.drawLine(106, y + 49, 106, y + 70, 1, true);
  }
}

static void themesPage() {
  softPanel(30, 184, 420, 492, "THEME SELECT");
  const char* names[] = {"Paper", "Studio", "Grid", "Focus"};
  for (int i = 0; i < 4; ++i) {
    int y = 248 + i * 78;
    bool selected = static_cast<int>(theme) == i;
    pill(78, y, 320, names[i], selected);
  }
  text(UI_12_FONT_ID, 82, 616, "Use UP and DOWN to change the visual skin.", true);
}

static void draw(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) {
  holdPower();
  ++paintCount;
  if (page == Page::Overview) {
    overviewPage();
  } else {
    background();
    header();
    switch (page) {
      case Page::Metrics:
        metricsPage();
        break;
      case Page::Work:
        workflowPage();
        break;
      case Page::Themes:
        themesPage();
        break;
      default:
        break;
    }
    footer();
  }
  const HalDisplay::RefreshMode actualRefreshMode = firstPaint ? HalDisplay::FULL_REFRESH : refreshMode;
  renderer.displayBuffer(actualRefreshMode);
  firstPaint = false;
}

static void handleButtons() {
  bool changed = false;
  if (input.wasPressed(InputManager::BTN_RIGHT)) {
    page = static_cast<Page>((static_cast<uint8_t>(page) + 1) % static_cast<uint8_t>(Page::Count));
    setFeedback("NEXT");
    changed = true;
  }
  if (input.wasPressed(InputManager::BTN_LEFT)) {
    page = static_cast<Page>((static_cast<uint8_t>(page) + static_cast<uint8_t>(Page::Count) - 1) %
                             static_cast<uint8_t>(Page::Count));
    setFeedback("PREV");
    changed = true;
  }
  if (input.wasPressed(InputManager::BTN_UP)) {
    theme = static_cast<Theme>((static_cast<uint8_t>(theme) + 1) % static_cast<uint8_t>(Theme::Count));
    setFeedback("THEME");
    changed = true;
  }
  if (input.wasPressed(InputManager::BTN_DOWN)) {
    theme = static_cast<Theme>((static_cast<uint8_t>(theme) + static_cast<uint8_t>(Theme::Count) - 1) %
                               static_cast<uint8_t>(Theme::Count));
    setFeedback("THEME");
    changed = true;
  }
  if (input.wasPressed(InputManager::BTN_CONFIRM)) {
    setFeedback("REFRESH");
    changed = true;
  }
  if (input.wasPressed(InputManager::BTN_BACK)) {
    page = Page::Overview;
    setFeedback("HOME");
    changed = true;
  }
  if (changed) draw(HalDisplay::FAST_REFRESH);
}

static void sendScreenshot() {
  const uint8_t* fb = renderer.getFrameBuffer();
  const size_t size = renderer.getBufferSize();
  Serial.printf("SCREENSHOT_START:%u:%u:%u\n", static_cast<unsigned>(size), renderer.getDisplayWidth(),
                renderer.getDisplayHeight());
  Serial.write(fb, size);
  Serial.print("\nSCREENSHOT_END\n");
}

static void handleSerial() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      serialLine[serialLineLen] = '\0';
      if (strcmp(serialLine, "SCREENSHOT") == 0) {
        sendScreenshot();
      } else if (strcmp(serialLine, "PING") == 0) {
        Serial.println("PONG");
      } else if (serialLine[0] == '{') {
        char result[64];
        if (status.applyJson(serialLine, result, sizeof(result))) {
          Serial.printf("STATUS_OK rev=%" PRIu32 "\n", status.revision);
          setFeedback("USB");
          draw(HalDisplay::FAST_REFRESH);
        } else {
          Serial.printf("STATUS_ERR %s\n", result);
        }
      }
      serialLineLen = 0;
      continue;
    }
    if (serialLineLen < sizeof(serialLine) - 1) {
      serialLine[serialLineLen++] = c;
    }
  }
}

void setup() {
  holdPower();
  delay(300);
  Serial.begin(115200);
  Serial.println("NOMI XTEINK GFX PORTRAIT BOOT");
  status.setDefaults();
  input.begin();
  display.begin(false);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::Portrait);
  registerFonts();
  bridge.begin(&status);
  setFeedback("READY");
  draw();
}

void loop() {
  holdPower();
  input.update();
  handleSerial();
  if (bridge.poll()) {
    setFeedback("BLE");
    draw(HalDisplay::FAST_REFRESH);
  }
  handleButtons();
  delay(20);
}

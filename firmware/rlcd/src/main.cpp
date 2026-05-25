#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <SPI.h>
#include "ST7305_4p2_BW_DisplayDriver.h"
#include "ST73xxPins.h"
#include "U8g2_for_ST73XX.h"

#define DEVICE_NAME  "Nomi RLCD"

#define SERVICE_UUID "f4f688c2-613e-56a5-b115-d19a99d1b463"
#define RX_CHAR_UUID "74879a99-7275-5b33-9665-51519f328fa5"
#define TX_CHAR_UUID "830ac719-8dea-541c-8d18-5e8de4cd83dd"
#define INFO_CHAR_UUID "485d9275-a3ad-516d-a524-e284f0aafdb1"

#define BLE_BUF_SIZE 1024

static const int SCREEN_W = 400;
static const int SCREEN_H = 300;

static const ST73xxPins PINS{PIN_DC, PIN_CS, PIN_SCLK, PIN_SDIN, PIN_RST};
static ST7305_4p2_BW_DisplayDriver display(PINS, SPI);

class RotatedDisplay : public ST73XX_UI {
public:
    explicit RotatedDisplay(ST7305_4p2_BW_DisplayDriver &target)
        : ST73XX_UI(SCREEN_W, SCREEN_H), target_(target) {}

    void writePoint(uint x, uint y, bool enabled) override {
        if (x >= SCREEN_W || y >= SCREEN_H) return;
        target_.writePoint(SCREEN_H - 1 - y, x, enabled);
    }

    void writePoint(uint x, uint y, uint16_t color) override {
        writePoint(x, y, color != 0);
    }

private:
    ST7305_4p2_BW_DisplayDriver &target_;
};

static RotatedDisplay canvas(display);
static U8G2_FOR_ST73XX u8g2;

static NimBLECharacteristic *tx_char = nullptr;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
static bool ble_connected = false;
static bool need_advertise = false;

struct CodexStatus {
    char state[24] = "offline";
    char session[96] = "current session";
    char model[24] = "gpt-5.5";
    char effort[16] = "medium";
    char tier[16] = "fast";
    char event[28] = "boot";
    char clock[8] = "--:--";
    int context_pct = 0;
    int used_tokens_k = 0;
    int five_left = -1;
    int weekly_left = -1;
    bool valid = false;
};

static CodexStatus status;

static const char *state_label(const char *state) {
    if (strcmp(state, "active") == 0) return "ACTIVE";
    if (strcmp(state, "waiting_approval") == 0) return "APPROVAL";
    if (strcmp(state, "waiting_input") == 0) return "INPUT";
    if (strcmp(state, "failed") == 0 || strcmp(state, "error") == 0) return "FAILED";
    if (strcmp(state, "idle") == 0 || strcmp(state, "ready") == 0) return "READY";
    if (strcmp(state, "offline") == 0) return "OFFLINE";
    return "SYNC";
}

static int clamp_pct(int pct) {
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

static void text(int x, int y, const uint8_t *font, const char *s) {
    u8g2.setFont(font);
    u8g2.drawUTF8(x, y, s);
}

static void draw_inverted_label(int x, int y, int w, int h, const char *s) {
    canvas.drawFilledRectangle(x, y, x + w, y + h, ST7305_COLOR_BLACK);
    u8g2.setForegroundColor(ST7305_COLOR_WHITE);
    u8g2.setBackgroundColor(ST7305_COLOR_BLACK);
    text(x + 8, y + h - 8, u8g2_font_7x14B_mf, s);
    u8g2.setForegroundColor(ST7305_COLOR_BLACK);
    u8g2.setBackgroundColor(ST7305_COLOR_WHITE);
}

static void draw_bar(int x, int y, int w, int h, int pct) {
    pct = clamp_pct(pct);
    canvas.drawRectangle(x, y, x + w, y + h, ST7305_COLOR_BLACK);
    int fill_w = (w - 4) * pct / 100;
    if (fill_w > 0) {
        canvas.drawFilledRectangle(x + 2, y + 2, x + 2 + fill_w, y + h - 2, ST7305_COLOR_BLACK);
    }
    for (int i = 1; i < 5; ++i) {
        int sx = x + i * w / 5;
        canvas.drawFastVLine(sx, y, h, ST7305_COLOR_BLACK);
    }
}

static void draw_pips(int x, int y, int w, int h, int pct) {
    pct = clamp_pct(pct);
    int gap = 3;
    int cell_w = (w - gap * 9) / 10;
    int filled = (pct + 9) / 10;
    for (int i = 0; i < 10; ++i) {
        int cx = x + i * (cell_w + gap);
        canvas.drawRectangle(cx, y, cx + cell_w, y + h, ST7305_COLOR_BLACK);
        if (i < filled) {
            canvas.drawFilledRectangle(cx + 2, y + 2, cx + cell_w - 2, y + h - 2, ST7305_COLOR_BLACK);
        }
    }
}

static void draw_top_bar() {
    canvas.drawFilledRectangle(10, 12, 22, 24, ST7305_COLOR_BLACK);
    canvas.drawRectangle(28, 12, 40, 24, ST7305_COLOR_BLACK);
    canvas.drawRectangle(46, 12, 58, 24, ST7305_COLOR_BLACK);
    text(68, 28, u8g2_font_9x18B_mf, "CODEX");
    text(256, 28, u8g2_font_7x14B_mf, ble_connected ? "BLE:LINK" : "BLE:ADV");
    text(348, 28, u8g2_font_7x14B_mf, status.clock);

    canvas.drawFastHLine(8, 38, 384, ST7305_COLOR_BLACK);
    text(12, 57, u8g2_font_wqy12_t_gb2312, status.session);
}

static void draw_governance_panel() {
    char line[80];
    canvas.drawRectangle(8, 66, 392, 132, ST7305_COLOR_BLACK);
    draw_inverted_label(20, 78, 96, 22, "SYSTEM");

    const char *label = state_label(status.state);
    u8g2.setFont(u8g2_font_courB24_tf);
    int w = u8g2.getUTF8Width(label);
    text(392 - w - 20, 112, u8g2_font_courB24_tf, label);

    snprintf(line, sizeof(line), "%s / %s / %s", status.model, status.effort, status.tier);
    text(22, 126, u8g2_font_7x14B_mf, line);
}

static void draw_metric_card(int x, int y, int w, const char *label, int pct, const char *sub) {
    char pct_line[16];
    pct = clamp_pct(pct);
    canvas.drawRectangle(x, y, x + w, y + 108, ST7305_COLOR_BLACK);
    text(x + 12, y + 20, u8g2_font_7x14B_mf, label);
    snprintf(pct_line, sizeof(pct_line), "%d%%", pct);
    text(x + 12, y + 62, u8g2_font_courB24_tf, pct_line);
    if (sub && sub[0]) {
        text(x + 12, y + 86, u8g2_font_6x12_mf, sub);
    }
    draw_pips(x + 12, y + 91, w - 24, 10, pct);
}

static void draw_resource_panel() {
    char token_line[24];
    snprintf(token_line, sizeof(token_line), "%dK TOKENS", status.used_tokens_k);
    text(12, 154, u8g2_font_7x14B_mf, "RESOURCE GOVERNANCE");
    canvas.drawFastHLine(12, 160, 376, ST7305_COLOR_BLACK);
    draw_metric_card(8, 170, 124, "CONTEXT", status.context_pct, token_line);
    draw_metric_card(138, 170, 124, "5H WINDOW", status.five_left, "REMAINING");
    draw_metric_card(268, 170, 124, "WEEKLY", status.weekly_left, "REMAINING");
}

static void draw_event_footer() {
    char line[64];
    canvas.drawFastHLine(8, 284, 384, ST7305_COLOR_BLACK);
    snprintf(line, sizeof(line), "EVENT: %s", status.event);
    text(12, 299, u8g2_font_6x12_mf, line);
}

static void draw_status() {
    display.clearDisplay();
    u8g2.setFontMode(1);
    u8g2.setForegroundColor(ST7305_COLOR_BLACK);
    u8g2.setBackgroundColor(ST7305_COLOR_WHITE);

    draw_top_bar();
    draw_governance_panel();

    if (!status.valid) {
        text(78, 198, u8g2_font_9x18B_mf, "WAITING FOR BRIDGE");
        text(108, 222, u8g2_font_7x14B_mf, ble_connected ? "BLE CONNECTED" : "BLE ADVERTISING");
        display.display();
        return;
    }

    draw_resource_panel();
    draw_event_footer();
    display.display();
}

static bool parse_status_json(const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    strlcpy(status.state, doc["state"] | "unknown", sizeof(status.state));
    strlcpy(status.session, doc["session"] | "current session", sizeof(status.session));
    strlcpy(status.model, doc["model"] | "unknown", sizeof(status.model));
    strlcpy(status.effort, doc["effort"] | "", sizeof(status.effort));
    strlcpy(status.tier, doc["tier"] | "", sizeof(status.tier));
    strlcpy(status.event, doc["event"] | "", sizeof(status.event));
    strlcpy(status.clock, doc["time"] | "--:--", sizeof(status.clock));
    status.context_pct = doc["context_pct"] | 0;
    status.used_tokens_k = doc["used_tokens_k"] | 0;
    JsonObject quota = doc["quota"].as<JsonObject>();
    status.five_left = quota["five_hour_left"] | doc["five_left"] | -1;
    status.weekly_left = quota["weekly_left"] | doc["weekly_left"] | -1;
    status.valid = true;
    return true;
}

static void start_advertising() {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->reset();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setName(DEVICE_NAME);
    adv->enableScanResponse(true);
    adv->start();
    Serial.println("BLE advertising");
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        ble_connected = true;
        Serial.printf("BLE connected from %s\n", info.getAddress().toString().c_str());
        draw_status();
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
        ble_connected = false;
        need_advertise = true;
        Serial.printf("BLE disconnected reason=%d\n", reason);
        draw_status();
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
        std::string value = chr->getValue();
        size_t len = value.length();
        if (len >= BLE_BUF_SIZE) len = BLE_BUF_SIZE - 1;
        memcpy(rx_buf, value.data(), len);
        rx_buf[len] = '\0';
        data_ready = true;
    }
};

static void init_ble() {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEServer *server = NimBLEDevice::createServer();
    static ServerCallbacks server_callbacks;
    server->setCallbacks(&server_callbacks);

    NimBLEService *svc = server->createService(SERVICE_UUID);
    NimBLECharacteristic *rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rx_callbacks;
    rx_char->setCallbacks(&rx_callbacks);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    tx_char->setValue("{\"rlcd\":true}");

    NimBLECharacteristic *info_char = svc->createCharacteristic(
        INFO_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );
    info_char->setValue("{\"protocol\":\"nomi-agent-display\",\"version\":1,\"device\":\"rlcd\",\"width\":400,\"height\":300}");

    svc->start();
    server->start();
    start_advertising();
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("Nomi RLCD Status boot");
    Serial.println("UI RLCD V1");

    SPI.begin(PIN_SCLK, -1, PIN_SDIN, PIN_CS);
    display.initialize();
    display.High_Power_Mode();
    display.display_on(true);
    display.display_Inversion(false);
    u8g2.begin(canvas);

    draw_status();
    init_ble();
    draw_status();
}

void loop() {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }

    if (data_ready) {
        data_ready = false;
        Serial.printf("BLE RX: %s\n", rx_buf);
        if (parse_status_json(rx_buf)) {
            draw_status();
            if (tx_char) {
                tx_char->setValue("{\"ack\":true}");
                tx_char->notify();
            }
        } else if (tx_char) {
            tx_char->setValue("{\"err\":true}");
            tx_char->notify();
        }
    }

    delay(20);
}

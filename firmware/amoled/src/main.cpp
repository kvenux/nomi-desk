#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <lvgl.h>

LV_FONT_DECLARE(font_noto_sc_22);

#define LCD_WIDTH   480
#define LCD_HEIGHT  480

#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   2

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[LCD_WIDTH * 40];
static lv_color_t buf2[LCD_WIDTH * 40];

static lv_obj_t *prompt_card = nullptr;
static lv_obj_t *prompt_glow[3] = {};
static lv_obj_t *prompt_rails[4] = {};
static lv_obj_t *root = nullptr;
static lv_obj_t *clock_chip = nullptr;
static lv_obj_t *ble_chip = nullptr;
static lv_obj_t *state_label_obj = nullptr;
static lv_obj_t *model_label_obj = nullptr;
static lv_obj_t *prompt_text_obj = nullptr;
static lv_obj_t *footer_label_obj = nullptr;
static lv_obj_t *metric_value_objs[3] = {};
static lv_obj_t *metric_bar_objs[3] = {};

#define BTN_TOP         0
#define BTN_BOTTOM      18
#define IIC_SDA         15
#define IIC_SCL         14
#define AXP2101_ADDR    0x34

#define DEVICE_NAME "Nomi AMOLED"
#define SERVICE_UUID "f4f688c2-613e-56a5-b115-d19a99d1b463"
#define RX_CHAR_UUID "74879a99-7275-5b33-9665-51519f328fa5"
#define TX_CHAR_UUID "830ac719-8dea-541c-8d18-5e8de4cd83dd"
#define INFO_CHAR_UUID "485d9275-a3ad-516d-a524-e284f0aafdb1"
#define BLE_BUF_SIZE 1024

static NimBLECharacteristic *tx_char = nullptr;
static NimBLEHIDDevice *hid_dev = nullptr;
static NimBLECharacteristic *input_kbd = nullptr;
static char rx_buf[BLE_BUF_SIZE];
static char serial_buf[BLE_BUF_SIZE];
static size_t serial_len = 0;
static volatile bool data_ready = false;
static bool ble_connected = false;
static bool need_advertise = false;
static XPowersPMU pmu;
static bool pmu_ok = false;

struct CodexStatus {
    char state[24] = "offline";
    char model[24] = "gpt-5.5";
    char effort[16] = "medium";
    char tier[16] = "fast";
    char event[32] = "boot";
    char prompt[128] = "waiting for prompt";
    char clock[8] = "--:--";
    int context_pct = 0;
    int used_tokens_k = 0;
    int five_left = -1;
    int weekly_left = -1;
    bool valid = false;
};

static CodexStatus status;

static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, 0x01,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,
};

static lv_color_t accent_green() { return lv_color_hex(0x00F0A8); }
static lv_color_t accent_cyan() { return lv_color_hex(0x37C7FF); }
static lv_color_t accent_pink() { return lv_color_hex(0xFF7AD9); }
static lv_color_t accent_amber() { return lv_color_hex(0xFFCC66); }
static lv_color_t panel_bg() { return lv_color_hex(0x080D11); }
static lv_color_t panel_line() { return lv_color_hex(0x20323A); }
static lv_color_t text_main() { return lv_color_hex(0xF4F8FA); }
static lv_color_t text_muted() { return lv_color_hex(0x7D98A4); }

static void flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    lv_disp_flush_ready(disp);
}

static void style_panel(lv_obj_t *obj, lv_color_t border, int radius = 10) {
    lv_obj_set_style_bg_color(obj, panel_bg(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    return obj;
}

static const char *display_state(const char *state) {
    if (strcmp(state, "active") == 0) return "WORKING";
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

static void copy_prompt(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) return;
    if (!src || !src[0]) {
        strlcpy(dest, "waiting for prompt", dest_size);
        return;
    }
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dest_size; ++i) {
        char ch = src[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        dest[out++] = ch;
    }
    dest[out] = '\0';
}

static bool is_working_state() {
    return strcmp(status.state, "active") == 0;
}

static void set_prompt_effect_enabled(bool enabled) {
    if (!prompt_card) return;
    if (!enabled) {
        lv_obj_set_style_border_color(prompt_card, panel_line(), 0);
        lv_obj_set_style_shadow_color(prompt_card, panel_line(), 0);
        lv_obj_set_style_shadow_opa(prompt_card, LV_OPA_10, 0);
    } else {
        lv_obj_set_style_shadow_opa(prompt_card, LV_OPA_30, 0);
    }
    for (int i = 0; i < 3; ++i) {
        if (!prompt_glow[i]) continue;
        if (enabled) {
            lv_obj_clear_flag(prompt_glow[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(prompt_glow[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (!prompt_rails[i]) continue;
        if (enabled) {
            lv_obj_clear_flag(prompt_rails[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(prompt_rails[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, lv_color_t border, lv_color_t color, int w) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, w, 30);
    style_panel(box, border, 6);
    lv_obj_set_style_pad_all(box, 0, 0);

    lv_obj_t *t = label(box, text, &lv_font_montserrat_14, color);
    lv_obj_center(t);
    return box;
}

static void make_bar(lv_obj_t *parent, int pct, lv_color_t color) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x071013), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x2B3D46), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
}

static lv_obj_t *metric_row(lv_obj_t *parent, int idx, const char *name, const char *value, int pct, lv_color_t color, int y) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 404, 36);
    lv_obj_set_pos(row, 22, y);
    style_panel(row, lv_color_hex(0x253944), 8);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x071013), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_shadow_color(row, color, 0);
    lv_obj_set_style_shadow_width(row, 6, 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_20, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(row, name, &lv_font_montserrat_14, text_muted());
    lv_obj_set_pos(title, 12, 9);

    metric_value_objs[idx] = label(row, value, &lv_font_montserrat_16, lv_color_hex(0xF2C9EC));
    lv_obj_set_width(metric_value_objs[idx], 112);
    lv_label_set_long_mode(metric_value_objs[idx], LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(metric_value_objs[idx], 120, 8);

    metric_bar_objs[idx] = lv_bar_create(row);
    lv_obj_set_size(metric_bar_objs[idx], 154, 8);
    lv_obj_set_pos(metric_bar_objs[idx], 238, 14);
    lv_bar_set_range(metric_bar_objs[idx], 0, 100);
    lv_bar_set_value(metric_bar_objs[idx], pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(metric_bar_objs[idx], lv_color_hex(0x111C22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(metric_bar_objs[idx], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(metric_bar_objs[idx], lv_color_hex(0x2B3D46), LV_PART_MAIN);
    lv_obj_set_style_border_width(metric_bar_objs[idx], 1, LV_PART_MAIN);
    lv_obj_set_style_radius(metric_bar_objs[idx], 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(metric_bar_objs[idx], color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(metric_bar_objs[idx], LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(metric_bar_objs[idx], 4, LV_PART_INDICATOR);
    return row;
}

static void create_prompt_card(lv_obj_t *parent) {
    for (int i = 0; i < 3; ++i) {
        prompt_glow[i] = lv_obj_create(parent);
        lv_obj_set_size(prompt_glow[i], 416 + i * 14, 114 + i * 14);
        lv_obj_set_pos(prompt_glow[i], 16 - i * 7, 76 - i * 7);
        lv_obj_clear_flag(prompt_glow[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(prompt_glow[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(prompt_glow[i], i == 0 ? 2 : 1, 0);
        lv_obj_set_style_border_color(prompt_glow[i], accent_green(), 0);
        lv_obj_set_style_border_opa(prompt_glow[i], LV_OPA_30, 0);
        lv_obj_set_style_radius(prompt_glow[i], 14 + i * 3, 0);
        lv_obj_set_style_shadow_width(prompt_glow[i], 32 + i * 12, 0);
        lv_obj_set_style_shadow_opa(prompt_glow[i], LV_OPA_30, 0);
        lv_obj_set_style_shadow_color(prompt_glow[i], accent_green(), 0);
    }

    prompt_card = lv_obj_create(parent);
    lv_obj_set_size(prompt_card, 404, 102);
    lv_obj_align(prompt_card, LV_ALIGN_TOP_MID, 0, 82);
    style_panel(prompt_card, panel_line(), 10);
    lv_obj_set_style_bg_color(prompt_card, lv_color_hex(0x0B151A), 0);
    lv_obj_set_style_shadow_width(prompt_card, 24, 0);
    lv_obj_set_style_shadow_opa(prompt_card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(prompt_card, accent_green(), 0);
    lv_obj_set_style_pad_left(prompt_card, 16, 0);
    lv_obj_set_style_pad_right(prompt_card, 16, 0);
    lv_obj_set_style_pad_top(prompt_card, 12, 0);

    lv_obj_t *title = label(prompt_card, "TASK Prompt", &lv_font_montserrat_16, accent_cyan());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    prompt_text_obj = label(prompt_card, status.prompt, &font_noto_sc_22, lv_color_hex(0xFFF06A));
    lv_obj_set_width(prompt_text_obj, 370);
    lv_label_set_long_mode(prompt_text_obj, LV_LABEL_LONG_DOT);
    lv_obj_align(prompt_text_obj, LV_ALIGN_TOP_LEFT, 0, 36);

    static const int rail_pos[4][4] = {
        {18, 78, 412, 10},
        {18, 180, 412, 10},
        {18, 78, 10, 112},
        {420, 78, 10, 112},
    };

    for (int i = 0; i < 4; ++i) {
        prompt_rails[i] = lv_obj_create(parent);
        lv_obj_set_pos(prompt_rails[i], rail_pos[i][0], rail_pos[i][1]);
        lv_obj_set_size(prompt_rails[i], rail_pos[i][2], rail_pos[i][3]);
        lv_obj_clear_flag(prompt_rails[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(prompt_rails[i], 0, 0);
        lv_obj_set_style_radius(prompt_rails[i], 6, 0);
        lv_obj_set_style_bg_opa(prompt_rails[i], LV_OPA_70, 0);
        lv_obj_set_style_shadow_width(prompt_rails[i], 24, 0);
        lv_obj_set_style_shadow_opa(prompt_rails[i], LV_OPA_70, 0);
        lv_obj_set_style_bg_grad_dir(prompt_rails[i], i < 2 ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER, 0);
        lv_obj_move_foreground(prompt_rails[i]);
    }

}

static lv_color_t wheel(int i) {
    static const lv_color_t colors[] = {
        lv_color_hex(0x00F0A8),
        lv_color_hex(0x37C7FF),
        lv_color_hex(0xFF7AD9),
        lv_color_hex(0xFFCC66),
    };
    return colors[i % 4];
}

static void create_brand(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 148, 34);
    lv_obj_set_pos(box, 22, 16);
    style_panel(box, lv_color_hex(0x293D46), 8);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x071012), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_pos(dot, 12, 11);
    lv_obj_set_style_bg_color(dot, accent_green(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_shadow_color(dot, accent_green(), 0);
    lv_obj_set_style_shadow_width(dot, 18, 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_80, 0);

    lv_obj_t *brand = label(box, "CODEX", &lv_font_montserrat_20, lv_color_hex(0xF6CFFF));
    lv_obj_set_pos(brand, 34, 5);
}

static void marquee_timer(lv_timer_t *) {
    if (!is_working_state()) {
        return;
    }

    lv_color_t border = wheel((millis() / 350) % 4);
    lv_obj_set_style_border_color(prompt_card, border, 0);
    lv_obj_set_style_shadow_color(prompt_card, border, 0);

    for (int i = 0; i < 3; ++i) {
        lv_color_t c = wheel(i + (millis() / 420));
        lv_obj_set_style_border_color(prompt_glow[i], c, 0);
        lv_obj_set_style_shadow_color(prompt_glow[i], c, 0);
        lv_obj_set_style_border_opa(prompt_glow[i], i == 0 ? LV_OPA_50 : LV_OPA_20, 0);
        lv_obj_set_style_shadow_opa(prompt_glow[i], i == 0 ? LV_OPA_50 : LV_OPA_30, 0);
    }

    for (int i = 0; i < 4; ++i) {
        lv_color_t c1 = wheel(i + (millis() / 260));
        lv_color_t c2 = wheel(i + 1 + (millis() / 260));
        lv_obj_set_style_bg_color(prompt_rails[i], c1, 0);
        lv_obj_set_style_bg_grad_color(prompt_rails[i], c2, 0);
        lv_obj_set_style_shadow_color(prompt_rails[i], c1, 0);
    }
}

static void build_ui() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x030405), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    root = lv_obj_create(scr);
    lv_obj_set_size(root, 448, 448);
    lv_obj_center(root);
    style_panel(root, lv_color_hex(0x1F3037), 28);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    create_brand(root);

    clock_chip = chip(root, "--:--", lv_color_hex(0x273740), text_main(), 62);
    lv_obj_set_pos(clock_chip, 282, 18);
    ble_chip = chip(root, "BLE ADV", lv_color_hex(0x273740), text_muted(), 82);
    lv_obj_set_pos(ble_chip, 352, 18);

    create_prompt_card(root);

    lv_obj_t *status = lv_obj_create(root);
    lv_obj_set_size(status, 404, 58);
    lv_obj_set_pos(status, 22, 198);
    style_panel(status, lv_color_hex(0x00684E), 9);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x061011), 0);

    state_label_obj = label(status, "OFFLINE", &lv_font_montserrat_28, accent_green());
    lv_obj_align(state_label_obj, LV_ALIGN_LEFT_MID, 12, 0);

    model_label_obj = label(status, "gpt-5.5  /  medium  /  fast", &lv_font_montserrat_16, lv_color_hex(0xF6CFFF));
    lv_obj_align(model_label_obj, LV_ALIGN_RIGHT_MID, -12, 1);

    metric_row(root, 0, "Context", "0% / 0K", 0, accent_green(), 266);
    metric_row(root, 1, "5H Window", "--% left", 0, accent_amber(), 308);
    metric_row(root, 2, "Weekly Limit", "--% left", 0, accent_pink(), 350);

    footer_label_obj = label(root, "EVENT: boot", &lv_font_montserrat_12, text_muted());
    lv_obj_set_pos(footer_label_obj, 34, 420);

    lv_timer_create(marquee_timer, 25, nullptr);
}

static void update_metric(int idx, int pct, const char *suffix) {
    char value[28];
    if (pct < 0) {
        snprintf(value, sizeof(value), "-- %s", suffix);
        pct = 0;
    } else {
        pct = clamp_pct(pct);
        snprintf(value, sizeof(value), "%d%% %s", pct, suffix);
    }
    lv_label_set_text(metric_value_objs[idx], value);
    lv_bar_set_value(metric_bar_objs[idx], pct, LV_ANIM_OFF);
}

static void refresh_ui() {
    char line[80];
    char detail[20];
    lv_label_set_text(state_label_obj, display_state(status.state));
    set_prompt_effect_enabled(is_working_state());
    snprintf(line, sizeof(line), "%s  /  %s  /  %s", status.model, status.effort, status.tier);
    lv_label_set_text(model_label_obj, line);
    lv_obj_align(model_label_obj, LV_ALIGN_RIGHT_MID, -12, 1);
    lv_label_set_text(prompt_text_obj, status.prompt);
    lv_label_set_text(lv_obj_get_child(clock_chip, 0), status.clock);
    lv_label_set_text(lv_obj_get_child(ble_chip, 0), ble_connected ? "BLE LINK" : "BLE ADV");
    lv_obj_set_style_border_color(ble_chip, ble_connected ? lv_color_hex(0x009870) : lv_color_hex(0x273740), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(ble_chip, 0), ble_connected ? accent_green() : text_muted(), 0);

    snprintf(detail, sizeof(detail), "/ %dK", status.used_tokens_k);
    update_metric(0, status.context_pct, detail);
    update_metric(1, status.five_left, "left");
    update_metric(2, status.weekly_left, "left");
    snprintf(line, sizeof(line), "EVENT: %s", status.event);
    lv_label_set_text(footer_label_obj, line);
}

static bool parse_status_json(const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    strlcpy(status.state, doc["state"] | "unknown", sizeof(status.state));
    strlcpy(status.model, doc["model"] | "unknown", sizeof(status.model));
    strlcpy(status.effort, doc["effort"] | "", sizeof(status.effort));
    strlcpy(status.tier, doc["tier"] | "", sizeof(status.tier));
    strlcpy(status.event, doc["event"] | "", sizeof(status.event));
    copy_prompt(status.prompt, sizeof(status.prompt), doc["prompt"] | status.prompt);
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

static void notify_key(const char *key, const char *action) {
    if (!ble_connected || !tx_char) return;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"key\":\"%s\",\"action\":\"%s\"}", key, action);
    tx_char->setValue(payload);
    tx_char->notify();
}

static void init_pmu_key() {
    Wire.begin(IIC_SDA, IIC_SCL);
    pmu_ok = pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL);
    if (!pmu_ok) {
        Serial.println("AXP2101 init failed");
        return;
    }
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    Serial.println("AXP2101 init OK");
}

static bool pkey_short_pressed() {
    if (!pmu_ok) return false;
    static uint32_t last_poll = 0;
    uint32_t now = millis();
    if (now - last_poll < 50) return false;
    last_poll = now;
    pmu.getIrqStatus();
    bool pressed = pmu.isPekeyShortPressIrq();
    pmu.clearIrqStatus();
    return pressed;
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        ble_connected = true;
        Serial.printf("BLE connected from %s\n", info.getAddress().toString().c_str());
        refresh_ui();
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
        ble_connected = false;
        need_advertise = true;
        Serial.printf("BLE disconnected reason=%d\n", reason);
        refresh_ui();
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
    NimBLEDevice::setSecurityAuth(false, false, false);
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
    tx_char->setValue("{\"amoled\":true}");

    NimBLECharacteristic *info_char = svc->createCharacteristic(
        INFO_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );
    info_char->setValue("{\"protocol\":\"nomi-agent-display\",\"version\":1,\"device\":\"amoled\",\"width\":480,\"height\":480}");

    svc->start();
    server->start();
    start_advertising();
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("Nomi AMOLED LVGL demo boot");
    Serial.println("UI LVGL Nomi BLE status");

    gfx->begin();
    gfx->setBrightness(230);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    build_ui();
    init_ble();
    init_pmu_key();
    pinMode(BTN_TOP, INPUT_PULLUP);
    pinMode(BTN_BOTTOM, INPUT_PULLUP);
    refresh_ui();
}

void loop() {
    static uint32_t last = millis();
    static uint32_t last_log = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last);
    last = now;
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
    if (data_ready) {
        data_ready = false;
        Serial.printf("BLE RX: %s\n", rx_buf);
        if (parse_status_json(rx_buf)) {
            refresh_ui();
            if (tx_char) {
                tx_char->setValue("{\"ack\":true}");
                tx_char->notify();
            }
        } else if (tx_char) {
            tx_char->setValue("{\"err\":true}");
            tx_char->notify();
        }
    }
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            serial_buf[serial_len] = '\0';
            if (serial_len > 0) {
                Serial.printf("SERIAL RX: %s\n", serial_buf);
                if (parse_status_json(serial_buf)) {
                    refresh_ui();
                    Serial.println("SERIAL ACK");
                } else {
                    Serial.println("SERIAL ERR");
                }
            }
            serial_len = 0;
        } else if (serial_len < BLE_BUF_SIZE - 1) {
            serial_buf[serial_len++] = ch;
        } else {
            serial_len = 0;
            Serial.println("SERIAL OVF");
        }
    }
    {
        static bool top_raw_was = false;
        static bool top_stable = false;
        static uint32_t top_changed_at = 0;
        static bool bottom_raw_was = false;
        static bool bottom_stable = false;
        static uint32_t bottom_changed_at = 0;
        bool top_raw = digitalRead(BTN_TOP) == LOW;
        bool bottom_raw = digitalRead(BTN_BOTTOM) == LOW;

        if (pkey_short_pressed()) {
            notify_key("ctrl_u", "tap");
            Serial.println("KEY: Ctrl+U");
        }

        if (top_raw != top_raw_was) {
            top_raw_was = top_raw;
            top_changed_at = now;
        }

        if (top_raw != top_stable && now - top_changed_at >= 25) {
            top_stable = top_raw;
            if (top_stable) {
                notify_key("enter", "tap");
                Serial.println("KEY: Enter");
            }
        }

        if (bottom_raw != bottom_raw_was) {
            bottom_raw_was = bottom_raw;
            bottom_changed_at = now;
        }

        if (bottom_raw != bottom_stable && now - bottom_changed_at >= 25) {
            bottom_stable = bottom_raw;
            if (bottom_stable) {
                notify_key("capslock", "down");
                Serial.println("KEY: CapsLock down");
            } else {
                notify_key("capslock", "up");
                Serial.println("KEY: CapsLock up");
            }
        }
    }
    lv_timer_handler();
    if (now - last_log > 2000) {
        last_log = now;
        Serial.println("LVGL alive: Codex BLE status");
    }
    delay(5);
}

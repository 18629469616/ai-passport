// main/main.c — FoloToy AI Passport BLE HID Keyboard (VibeCoding / 微信语音控制器)
#include "bsp_i2c.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "ble_hid.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "main";

// UI 元素指针
static lv_obj_t *s_battery_label = NULL;
static lv_obj_t *s_conn_status_label = NULL;
static lv_obj_t *s_conn_hint_label = NULL;
static lv_obj_t *s_action_label = NULL;
static lv_timer_t *s_status_timer = NULL;

static bool s_last_connected = false;
static int s_last_battery_soc = -1;

// ================================================================
// 按键响应回调 (运行在 button 驱动任务中，必须保持非阻塞)
// ================================================================
static void update_action_feedback(const char *text, uint32_t color_hex)
{
    if (bsp_lvgl_lock(200)) {
        if (s_action_label) {
            lv_label_set_text(s_action_label, text);
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(color_hex), 0);
        }
        bsp_lvgl_unlock();
    }
}

static void on_button_event(bsp_btn_t btn, bsp_btn_ev_t ev, void *arg)
{
    (void)arg;

    switch (btn) {
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "UP Click -> Left Ctrl + F10 (WeChat Voice)");
            ble_hid_key_press_mod(HID_MOD_LEFT_CTRL, HID_KEY_F10);
            update_action_feedback("[Voice] Ctrl+F10", 0x4ADE80);
        } else if (ev == BSP_BTN_DOUBLE) {
            ESP_LOGI(TAG, "UP Double Click -> Win + H (Win Dictation)");
            ble_hid_key_press_mod(HID_MOD_LEFT_GUI, HID_KEY_H);
            update_action_feedback("[Win Voice] Win+H", 0x60A5FA);
        } else if (ev == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "UP Long -> Left Ctrl + Left Win");
            ble_hid_key_press_mod(HID_MOD_LEFT_CTRL | HID_MOD_LEFT_GUI, HID_KEY_NONE);
            update_action_feedback("[Voice] Ctrl+Win", 0xA78BFA);
        }
        break;

    // 中间物理按键 (硬件分压 1k，驱动产生 BSP_BTN_DOWN)
    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "MID Click -> Enter (Submit Prompt)");
            ble_hid_key_press(HID_KEY_RETURN);
            update_action_feedback("[Send] Enter", 0x38BDF8);
        } else if (ev == BSP_BTN_DOUBLE) {
            ESP_LOGI(TAG, "MID Double Click -> Shift + Enter (Newline)");
            ble_hid_key_press_mod(HID_MOD_LEFT_SHIFT, HID_KEY_RETURN);
            update_action_feedback("[Newline] Shift+Enter", 0x818CF8);
        } else if (ev == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "MID Long -> Escape (Cancel / Exit)");
            ble_hid_key_press(HID_KEY_ESCAPE);
            update_action_feedback("[Cancel] Escape", 0xF472B6);
        }
        break;

    // 最底物理按键 (硬件分压 2.2k，驱动产生 BSP_BTN_OK)
    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "DOWN Click -> Backspace (Del Char)");
            ble_hid_key_press(HID_KEY_BACKSPACE);
            update_action_feedback("[Del] Backspace", 0xFB923C);
        } else if (ev == BSP_BTN_DOUBLE) {
            ESP_LOGI(TAG, "DOWN Double Click -> Ctrl + Backspace (Del Word)");
            ble_hid_key_press_mod(HID_MOD_LEFT_CTRL, HID_KEY_BACKSPACE);
            update_action_feedback("[Del Word] Ctrl+BS", 0xF97316);
        } else if (ev == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "DOWN Long -> Clear All (Ctrl+A then Backspace)");
            ble_hid_clear_all_text();
            update_action_feedback("[Clear All] Ctrl+A+BS", 0xEF4444);
        }
        break;
    }
}

// ================================================================
// 定时状态刷新 (每 500ms 检查连接和电量)
// ================================================================
static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    bool connected = ble_hid_is_connected();
    if (connected != s_last_connected) {
        s_last_connected = connected;
        if (connected) {
            char peer_str[24] = {0};
            ble_hid_get_peer_str(peer_str, sizeof(peer_str));
            lv_label_set_text(s_conn_status_label, "CONNECTED");
            lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0x00E676), 0);
            if (peer_str[0]) {
                lv_label_set_text_fmt(s_conn_hint_label, "Host: %s", peer_str);
            } else {
                lv_label_set_text(s_conn_hint_label, "Ready for VibeCoding");
            }
            lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0x8892B0), 0);
            lv_label_set_text(s_action_label, "Connected");
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x00E676), 0);
        } else {
            lv_label_set_text(s_conn_status_label, "WAITING...");
            lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0xFFB300), 0);
            lv_label_set_text(s_conn_hint_label, "Pair: Passport-Key");
            lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0xCBD5E1), 0);
            lv_label_set_text(s_action_label, "Waiting Pairing");
            lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x94A3B8), 0);
        }
    }

    // 电量检测与更新
    int soc = bsp_battery_soc();
    if (soc >= 0 && soc != s_last_battery_soc) {
        s_last_battery_soc = soc;
        lv_label_set_text_fmt(s_battery_label, "BAT %d%%", soc);
        ble_hid_set_battery((uint8_t)soc);
    }
}

// ================================================================
// 构建 UI (屏幕 240x320)
// ================================================================
static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F172A), 0);

    // 顶部状态栏: 设备名与电量
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Passport-Key");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    s_battery_label = lv_label_create(scr);
    lv_label_set_text(s_battery_label, "BAT --");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    // 卡片 1: 蓝牙连接状态 (Y: 42, H: 86)
    lv_obj_t *card_conn = lv_obj_create(scr);
    lv_obj_set_size(card_conn, 216, 86);
    lv_obj_align(card_conn, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card_conn, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(card_conn, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(card_conn, 1, 0);
    lv_obj_set_style_radius(card_conn, 10, 0);
    lv_obj_set_scrollbar_mode(card_conn, LV_SCROLLBAR_MODE_OFF);

    s_conn_status_label = lv_label_create(card_conn);
    lv_label_set_text(s_conn_status_label, "WAITING...");
    lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(s_conn_status_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_conn_status_label, LV_ALIGN_TOP_MID, 0, 6);

    s_conn_hint_label = lv_label_create(card_conn);
    lv_label_set_text(s_conn_hint_label, "Pair: Passport-Key");
    lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_font(s_conn_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_conn_hint_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 卡片 2: 按键操作指南 (Y: 138, H: 120)
    lv_obj_t *card_guide = lv_obj_create(scr);
    lv_obj_set_size(card_guide, 216, 120);
    lv_obj_align(card_guide, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_bg_color(card_guide, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(card_guide, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(card_guide, 1, 0);
    lv_obj_set_style_radius(card_guide, 10, 0);
    lv_obj_set_scrollbar_mode(card_guide, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *hint_up = lv_label_create(card_guide);
    lv_label_set_text(hint_up, "UP   | Voice (Ctrl+F10)");
    lv_obj_set_style_text_color(hint_up, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_text_font(hint_up, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_up, LV_ALIGN_TOP_LEFT, 6, 8);

    lv_obj_t *hint_ok = lv_label_create(card_guide);
    lv_label_set_text(hint_ok, "MID  | Enter / Shift+Ent");
    lv_obj_set_style_text_color(hint_ok, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(hint_ok, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_ok, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *hint_dn = lv_label_create(card_guide);
    lv_label_set_text(hint_dn, "DOWN | Backspace / Clr");
    lv_obj_set_style_text_color(hint_dn, lv_color_hex(0xFB923C), 0);
    lv_obj_set_style_text_font(hint_dn, &lv_font_montserrat_14, 0);
    lv_obj_align(hint_dn, LV_ALIGN_BOTTOM_LEFT, 6, -8);

    // 底部卡片 3: 实时操作反馈 (Y: 266, H: 42)
    lv_obj_t *card_act = lv_obj_create(scr);
    lv_obj_set_size(card_act, 216, 42);
    lv_obj_align(card_act, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(card_act, lv_color_hex(0x020617), 0);
    lv_obj_set_style_border_color(card_act, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(card_act, 1, 0);
    lv_obj_set_style_radius(card_act, 8, 0);
    lv_obj_set_scrollbar_mode(card_act, LV_SCROLLBAR_MODE_OFF);

    s_action_label = lv_label_create(card_act);
    lv_label_set_text(s_action_label, "Ready");
    lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(s_action_label, &lv_font_montserrat_14, 0);
    lv_obj_center(s_action_label);

    s_status_timer = lv_timer_create(status_timer_cb, 500, NULL);
}

// ================================================================
// 主入口
// ================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "=== FoloToy AI Passport BLE HID Keyboard Starting ===");

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 I2C 总线
    bsp_i2c_init();

    // 3. 初始化 CW2017 电池电量计
    if (bsp_battery_init() == ESP_OK) {
        ESP_LOGI(TAG, "Battery gauge CW2017 ready");
    } else {
        ESP_LOGW(TAG, "Battery gauge CW2017 not detected");
    }

    // 4. 初始化 ST7789P3 屏幕与 LVGL
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display / LVGL init failed");
        return;
    }
    bsp_display_backlight(100);

    // 5. 构建 UI 界面
    if (bsp_lvgl_lock(1000)) {
        build_ui();
        bsp_lvgl_unlock();
    }

    // 6. 初始化按键驱动
    if (bsp_button_init(on_button_event, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed");
        return;
    }

    // 7. 初始化 BLE HID 服务
    esp_err_t err = ble_hid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID init failed: %d", err);
        if (bsp_lvgl_lock(1000)) {
            lv_label_set_text(s_conn_status_label, "BLE INIT FAIL");
            lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0xEF4444), 0);
            bsp_lvgl_unlock();
        }
        return;
    }

    ESP_LOGI(TAG, "BLE HID Keyboard initialized. Advertising as 'Passport-Key'...");
}

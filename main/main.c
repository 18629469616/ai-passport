// main/main.c — FoloToy AI Passport BLE HID Keyboard (Linear Dark Tech Edition)
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
static lv_obj_t *s_battery_bar = NULL;
static lv_obj_t *s_conn_status_label = NULL;
static lv_obj_t *s_conn_hint_label = NULL;
static lv_obj_t *s_counter_val_label = NULL;
static lv_obj_t *s_action_val_label = NULL;
static lv_timer_t *s_status_timer = NULL;

// 状态变量
static bool s_last_connected = false;
static int s_last_battery_soc = -1;
static bool s_recording = false;
static uint32_t s_record_start_tick = 0;
static uint32_t s_prompt_count = 0;
static uint32_t s_last_active_tick = 0;
static uint8_t s_current_brightness = 100;

// ================================================================
// 背光快速节能休眠管理 (10秒变暗，20秒彻底熄屏，按键瞬间唤醒)
// ================================================================
static void reset_activity(void)
{
    s_last_active_tick = xTaskGetTickCount();
    if (s_current_brightness != 100) {
        s_current_brightness = 100;
        bsp_display_backlight(100);
    }
}

static void update_power_saving(void)
{
    // 如果正在录音说话，保持常亮
    if (s_recording) {
        reset_activity();
        return;
    }

    uint32_t elapsed_sec = (xTaskGetTickCount() - s_last_active_tick) / configTICK_RATE_HZ;

    // 20 秒无操作 -> 彻底关闭背光熄屏（黑屏省电，功耗降 80%）
    if (elapsed_sec >= 20) {
        if (s_current_brightness != 0) {
            s_current_brightness = 0;
            bsp_display_backlight(0);
        }
    }
    // 10 秒无操作 -> 调暗至 15%
    else if (elapsed_sec >= 10) {
        if (s_current_brightness != 15) {
            s_current_brightness = 15;
            bsp_display_backlight(15);
        }
    }
}

// ================================================================
// 实时按键操作反馈更新
// ================================================================
static void update_action_feedback(const char *text, uint32_t color_hex)
{
    if (bsp_lvgl_lock(200)) {
        if (s_action_val_label) {
            lv_label_set_text(s_action_val_label, text);
            lv_obj_set_style_text_color(s_action_val_label, lv_color_hex(color_hex), 0);
        }
        bsp_lvgl_unlock();
    }
}

static void on_button_event(bsp_btn_t btn, bsp_btn_ev_t ev, void *arg)
{
    (void)arg;
    reset_activity(); // 唤醒屏幕并重置休眠计时

    switch (btn) {
    // 顶部物理按键：微信语音
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "UP Click -> Left Ctrl + F10 (WeChat Voice)");
            ble_hid_key_press_mod(HID_MOD_LEFT_CTRL, HID_KEY_F10);
            s_recording = !s_recording;
            if (s_recording) {
                s_record_start_tick = xTaskGetTickCount();
                update_action_feedback("Voice On", 0x10B981);
            } else {
                update_action_feedback("Voice Off", 0x64748B);
            }
        } else if (ev == BSP_BTN_DOUBLE) {
            ESP_LOGI(TAG, "UP Double Click -> Win + H (Win Dictation)");
            ble_hid_key_press_mod(HID_MOD_LEFT_GUI, HID_KEY_H);
            update_action_feedback("Win+H Voice", 0x38BDF8);
        } else if (ev == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "UP Long -> Left Ctrl + Left Win");
            ble_hid_key_press_mod(HID_MOD_LEFT_CTRL | HID_MOD_LEFT_GUI, HID_KEY_NONE);
            update_action_feedback("Ctrl+Win", 0xA78BFA);
        }
        break;

    // 中间物理按键：确认发送 (驱动产生 BSP_BTN_DOWN)
    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "MID Click -> Enter (Submit Prompt)");
            ble_hid_key_press(HID_KEY_RETURN);
            s_recording = false;
            s_prompt_count++;
            update_action_feedback("Enter Send", 0x06B6D4);
        } else if (ev == BSP_BTN_DOUBLE) {
            ESP_LOGI(TAG, "MID Double Click -> Shift + Enter (Newline)");
            ble_hid_key_press_mod(HID_MOD_LEFT_SHIFT, HID_KEY_RETURN);
            update_action_feedback("Shift+Enter", 0x818CF8);
        } else if (ev == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "MID Long -> Escape (Cancel / Exit)");
            ble_hid_key_press(HID_KEY_ESCAPE);
            s_recording = false;
            update_action_feedback("Escape", 0xF472B6);
        }
        break;

    // 最底物理按键：删除退格 (驱动产生 BSP_BTN_OK)
    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "DOWN Click -> Backspace (Del Char)");
            ble_hid_key_press(HID_KEY_BACKSPACE);
            update_action_feedback("Backspace", 0xF59E0B);
        } else if (ev == BSP_BTN_DOUBLE) {
            ESP_LOGI(TAG, "DOWN Double Click -> Ctrl + Backspace (Del Word)");
            ble_hid_key_press_mod(HID_MOD_LEFT_CTRL, HID_KEY_BACKSPACE);
            update_action_feedback("Del Word", 0xFB923C);
        } else if (ev == BSP_BTN_LONG) {
            ESP_LOGI(TAG, "DOWN Long -> Clear All (Ctrl+A then Backspace)");
            ble_hid_clear_all_text();
            update_action_feedback("All Cleared", 0xEF4444);
        }
        break;
    }
}

// ================================================================
// 定时状态刷新 (每 500ms 刷新连接、电量、录音计时与省电)
// ================================================================
static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    update_power_saving();

    bool connected = ble_hid_is_connected();

    // 录音计时模式优先展示
    if (s_recording && connected) {
        uint32_t rec_sec = (xTaskGetTickCount() - s_record_start_tick) / configTICK_RATE_HZ;
        lv_label_set_text_fmt(s_conn_status_label, "%02lu:%02lu", (unsigned long)(rec_sec / 60), (unsigned long)(rec_sec % 60));
        lv_obj_set_style_text_font(s_conn_status_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0xEF4444), 0);
        lv_label_set_text(s_conn_hint_label, "Listening... Press MID to Send");
        lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0xFDE047), 0);
    } else {
        if (connected != s_last_connected || !s_recording) {
            s_last_connected = connected;
            lv_obj_set_style_text_font(s_conn_status_label, &lv_font_montserrat_20, 0);
            if (connected) {
                lv_label_set_text(s_conn_status_label, "CONNECTED");
                lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0x10B981), 0);
                lv_label_set_text(s_conn_hint_label, "Ready for VibeCoding");
                lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0x94A3B8), 0);
            } else {
                lv_label_set_text(s_conn_status_label, "WAITING");
                lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0xF59E0B), 0);
                lv_label_set_text(s_conn_hint_label, "Pair: Passport-Key");
                lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0x64748B), 0);
            }
        }
    }

    // 更新 Prompt 统计
    if (s_counter_val_label) {
        lv_label_set_text_fmt(s_counter_val_label, "# %lu", (unsigned long)s_prompt_count);
    }

    // 电量与电量条更新 (无重叠)
    int soc = bsp_battery_soc();
    if (soc >= 0 && soc != s_last_battery_soc) {
        s_last_battery_soc = soc;
        lv_label_set_text_fmt(s_battery_label, "%d%%", soc);
        if (s_battery_bar) {
            lv_bar_set_value(s_battery_bar, soc, LV_ANIM_ON);
            if (soc > 50) {
                lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x10B981), LV_PART_INDICATOR);
            } else if (soc > 20) {
                lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x06B6D4), LV_PART_INDICATOR);
            } else {
                lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0xEF4444), LV_PART_INDICATOR);
            }
        }
        ble_hid_set_battery((uint8_t)soc);
    }
}

// ================================================================
// 构建 UI (ST7789 屏幕 240x320)
// ================================================================
static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    // Linear 风格夜幕黑背景 (深沉高级，不刺眼)
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x08090A), 0);

    // ============================================================
    // 1. 顶部 Header (Y: 8 to 28)
    // ============================================================
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Passport-Key");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(title, 14, 10);

    // 电量数字 (靠最右，x=198)
    s_battery_label = lv_label_create(scr);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_battery_label, 196, 10);

    // 电量胶囊柱状图 (放在数字左侧，x=164, y=14, 宽24，高8，完全不重叠)
    s_battery_bar = lv_bar_create(scr);
    lv_obj_set_pos(s_battery_bar, 164, 14);
    lv_obj_set_size(s_battery_bar, 26, 7);
    lv_bar_set_range(s_battery_bar, 0, 100);
    lv_bar_set_value(s_battery_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x1E2638), 0);
    lv_obj_set_style_border_color(s_battery_bar, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_battery_bar, 1, 0);
    lv_obj_set_style_radius(s_battery_bar, 2, 0);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(0x10B981), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_battery_bar, 1, LV_PART_INDICATOR);

    // ============================================================
    // 2. 主状态展示卡片 (Y: 36, H: 86, W: 216, X: 12)
    // ============================================================
    lv_obj_t *card_conn = lv_obj_create(scr);
    lv_obj_set_pos(card_conn, 12, 36);
    lv_obj_set_size(card_conn, 216, 86);
    lv_obj_set_style_bg_color(card_conn, lv_color_hex(0x111622), 0);
    lv_obj_set_style_border_color(card_conn, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(card_conn, 1, 0);
    lv_obj_set_style_radius(card_conn, 10, 0);
    lv_obj_set_scrollbar_mode(card_conn, LV_SCROLLBAR_MODE_OFF);

    s_conn_status_label = lv_label_create(card_conn);
    lv_label_set_text(s_conn_status_label, "WAITING");
    lv_obj_set_style_text_color(s_conn_status_label, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_font(s_conn_status_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_conn_status_label, LV_ALIGN_TOP_MID, 0, 8);

    s_conn_hint_label = lv_label_create(card_conn);
    lv_label_set_text(s_conn_hint_label, "Pair: Passport-Key");
    lv_obj_set_style_text_color(s_conn_hint_label, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(s_conn_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_conn_hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    // ============================================================
    // 3. 极简按键指南卡片 (Y: 130, H: 114, W: 216, X: 12)
    // 两列排版，永不超屏，清爽干净！
    // ============================================================
    lv_obj_t *card_guide = lv_obj_create(scr);
    lv_obj_set_pos(card_guide, 12, 130);
    lv_obj_set_size(card_guide, 216, 114);
    lv_obj_set_style_bg_color(card_guide, lv_color_hex(0x111622), 0);
    lv_obj_set_style_border_color(card_guide, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(card_guide, 1, 0);
    lv_obj_set_style_radius(card_guide, 10, 0);
    lv_obj_set_scrollbar_mode(card_guide, LV_SCROLLBAR_MODE_OFF);

    // 行 1: UP 键
    lv_obj_t *tag_up = lv_label_create(card_guide);
    lv_label_set_text(tag_up, "UP");
    lv_obj_set_style_text_color(tag_up, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_text_font(tag_up, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(tag_up, 4, 8);

    lv_obj_t *act_up = lv_label_create(card_guide);
    lv_label_set_text(act_up, "Voice Dictation");
    lv_obj_set_style_text_color(act_up, lv_color_hex(0xF1F5F9), 0);
    lv_obj_set_style_text_font(act_up, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(act_up, 38, 8);

    // 行 2: MID 键
    lv_obj_t *tag_mid = lv_label_create(card_guide);
    lv_label_set_text(tag_mid, "MID");
    lv_obj_set_style_text_color(tag_mid, lv_color_hex(0x06B6D4), 0);
    lv_obj_set_style_text_font(tag_mid, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(tag_mid, 4, 40);

    lv_obj_t *act_mid = lv_label_create(card_guide);
    lv_label_set_text(act_mid, "Send / Newline");
    lv_obj_set_style_text_color(act_mid, lv_color_hex(0xF1F5F9), 0);
    lv_obj_set_style_text_font(act_mid, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(act_mid, 38, 40);

    // 行 3: DOWN 键
    lv_obj_t *tag_dn = lv_label_create(card_guide);
    lv_label_set_text(tag_dn, "DN");
    lv_obj_set_style_text_color(tag_dn, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_font(tag_dn, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(tag_dn, 4, 72);

    lv_obj_t *act_dn = lv_label_create(card_guide);
    lv_label_set_text(act_dn, "Delete / Clear");
    lv_obj_set_style_text_color(act_dn, lv_color_hex(0xF1F5F9), 0);
    lv_obj_set_style_text_font(act_dn, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(act_dn, 38, 72);

    // ============================================================
    // 4. 底部状态与 Prompt 统计卡片 (Y: 252, H: 54, W: 216, X: 12)
    // 左右分列，彻底杜绝文字重叠！
    // ============================================================
    lv_obj_t *card_act = lv_obj_create(scr);
    lv_obj_set_pos(card_act, 12, 252);
    lv_obj_set_size(card_act, 216, 54);
    lv_obj_set_style_bg_color(card_act, lv_color_hex(0x0D111A), 0);
    lv_obj_set_style_border_color(card_act, lv_color_hex(0x1E2638), 0);
    lv_obj_set_style_border_width(card_act, 1, 0);
    lv_obj_set_style_radius(card_act, 8, 0);
    lv_obj_set_scrollbar_mode(card_act, LV_SCROLLBAR_MODE_OFF);

    // 左侧：Prompt 统计
    lv_obj_t *lbl_counter_tag = lv_label_create(card_act);
    lv_label_set_text(lbl_counter_tag, "PROMPTS");
    lv_obj_set_style_text_color(lbl_counter_tag, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(lbl_counter_tag, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_counter_tag, 6, 2);

    s_counter_val_label = lv_label_create(card_act);
    lv_label_set_text(s_counter_val_label, "# 0");
    lv_obj_set_style_text_color(s_counter_val_label, lv_color_hex(0xA855F7), 0);
    lv_obj_set_style_text_font(s_counter_val_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_counter_val_label, 6, 20);

    // 右侧：实时反馈
    lv_obj_t *lbl_action_tag = lv_label_create(card_act);
    lv_label_set_text(lbl_action_tag, "ACTION");
    lv_obj_set_style_text_color(lbl_action_tag, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(lbl_action_tag, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_action_tag, 100, 2);

    s_action_val_label = lv_label_create(card_act);
    lv_label_set_text(s_action_val_label, "Ready");
    lv_obj_set_style_text_color(s_action_val_label, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(s_action_val_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_action_val_label, 100, 20);

    s_status_timer = lv_timer_create(status_timer_cb, 500, NULL);
}

// ================================================================
// 主入口
// ================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "=== FoloToy AI Passport BLE HID Keyboard (Linear Edition) ===");

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
    s_last_active_tick = xTaskGetTickCount();

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

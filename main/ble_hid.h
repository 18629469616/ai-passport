// main/ble_hid.h — BLE HID Keyboard 服务接口
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// HID 修饰键位图 (HID Usage Tables Page 0x07)
#define HID_MOD_NONE        0x00
#define HID_MOD_LEFT_CTRL   0x01
#define HID_MOD_LEFT_SHIFT  0x02
#define HID_MOD_LEFT_ALT    0x04
#define HID_MOD_LEFT_GUI    0x08  // Windows 键 / macOS Cmd
#define HID_MOD_RIGHT_CTRL  0x10
#define HID_MOD_RIGHT_SHIFT 0x20
#define HID_MOD_RIGHT_ALT   0x40
#define HID_MOD_RIGHT_GUI   0x80

// 常用按键 Usage ID
#define HID_KEY_NONE        0x00
#define HID_KEY_A           0x04
#define HID_KEY_H           0x0B
#define HID_KEY_RETURN      0x28  // Enter / 回车
#define HID_KEY_ESCAPE      0x29  // Esc
#define HID_KEY_BACKSPACE   0x2A  // Backspace / 退格
#define HID_KEY_TAB         0x2B  // Tab
#define HID_KEY_SPACE       0x2C  // 空格
#define HID_KEY_F1          0x3A
#define HID_KEY_F5          0x3E
#define HID_KEY_F10         0x43  // F10 (微信输入法语音)
#define HID_KEY_RIGHT_ARROW 0x4F
#define HID_KEY_LEFT_ARROW  0x50
#define HID_KEY_DOWN_ARROW  0x51
#define HID_KEY_UP_ARROW    0x52

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE HID Keyboard 服务
 * @return ESP_OK 成功
 */
esp_err_t ble_hid_init(void);

/**
 * @brief 发送单次按键（按下 + 释放），非阻塞队列投递
 * @param keycode HID Usage ID（如 HID_KEY_RETURN）
 */
void ble_hid_key_press(uint8_t keycode);

/**
 * @brief 发送带修饰键的组合键（按下 + 释放），非阻塞队列投递
 * @param modifier HID 修饰键掩码（如 HID_MOD_LEFT_CTRL）
 * @param keycode  HID Usage ID（如 HID_KEY_F10）
 */
void ble_hid_key_press_mod(uint8_t modifier, uint8_t keycode);

/**
 * @brief 发送全选并清空（Ctrl + A 之后按 Backspace），非阻塞队列投递
 */
void ble_hid_clear_all_text(void);

/**
 * @brief 更新并上报蓝牙 HID 电池电量 (0 - 100)
 * @param level 电量百分比
 */
void ble_hid_set_battery(uint8_t level);

/**
 * @brief 检查当前是否有主机已连接且完成加密认证
 * @return true 已连接可发送按键
 */
bool ble_hid_is_connected(void);

/**
 * @brief 获取已连接主机的 MAC 地址字符串（用于 UI 状态显示）
 * @param buf 输出缓冲区
 * @param len 缓冲区大小（建议 >= 18）
 * @return true 获取成功
 */
bool ble_hid_get_peer_str(char *buf, size_t len);

/**
 * @brief 重置蓝牙配对绑定信息并重启
 */
void ble_hid_reset_bonding(void);

/**
 * @brief 停止 BLE 广播
 */
void ble_hid_stop(void);

#ifdef __cplusplus
}
#endif

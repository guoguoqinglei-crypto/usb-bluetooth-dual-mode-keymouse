#ifndef _USB_GAMEPAD_H
#define _USB_GAMEPAD_H

#include <stdint.h>

/* 初始化 STM32F103 原生 USB HID 游戏手柄 */
uint8_t UsbGamepad_Init(void);

/* 发送一帧手柄报告：4 个轴和 16 位按键位图 */
uint8_t UsbGamepad_Send(int16_t x,
                        int16_t y,
                        int16_t rx,
                        int16_t ry,
                        uint16_t buttons);

/* 返回 USB 是否已经完成枚举并进入 Configured 状态 */
uint8_t UsbGamepad_IsConfigured(void);

#endif

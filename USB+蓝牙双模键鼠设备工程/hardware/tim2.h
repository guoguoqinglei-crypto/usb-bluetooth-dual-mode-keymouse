#ifndef _TIM2_H
#define _TIM2_H

#include <stdint.h>

// 初始化 TIM2 软件 PWM
void Tim2_Init(void);

// 设置红灯亮度，范围 0~100
void Tim2_SetRedBrightness(uint8_t brightness);

// 设置绿灯亮度，范围 0~100
void Tim2_SetGreenBrightness(uint8_t brightness);

// 设置蓝灯亮度，范围 0~100
void Tim2_SetBlueBrightness(uint8_t brightness);

#endif

#ifndef _DELAY_H
#define _DELAY_H

// 包含标准整数类型定义，例如 uint16_t
#include <stdint.h>
// 包含 STM32F10x 标准库头文件，提供 SysTick 寄存器定义
#include <stm32f10x.h>

// 微秒级延时函数
// 参数 us：要延时的微秒数，范围受限于 SysTick 24 位计数器
void Delay_us(uint16_t us);

// 毫秒级延时函数
// 参数 ms：要延时的毫秒数
void Delay_ms(uint16_t ms);

// 秒级延时函数
// 参数 s：要延时的秒数
void Delay_s(uint16_t s);

#endif

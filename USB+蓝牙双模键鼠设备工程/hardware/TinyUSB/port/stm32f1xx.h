/*
 * TinyUSB 的 STM32F1 移植层需要一个 CMSIS 设备头 stm32f1xx.h。
 * 本工程使用的是 ST 标准外设库自带的 stm32f10x.h，这里做一层转发，
 * 避免为了 TinyUSB 再引入一套 CMSIS 设备头文件。
 */
#ifndef TINYUSB_PORT_STM32F1XX_H
#define TINYUSB_PORT_STM32F1XX_H

#include <stm32f10x.h>

#endif

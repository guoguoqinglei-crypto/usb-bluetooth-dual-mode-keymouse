/**
  ******************************************************************************
  * @file    usb_cdc.h
  * @brief   USB CDC 虚拟串口（用板子原生 USB 口当调试串口，不需要 USB-TTL）
  *
  * 开关在 tusb_config.h 的 ENABLE_USB_CDC。打开后：
  *   - 电脑“端口(COM 和 LPT)”里出现一个 USB 串行设备（Win10/11 免驱）
  *   - UART1_SendString / UART1_Printf 的内容会同时从 USB 发一份，
  *     所以不接 USB-TTL 也能看到全部日志
  ******************************************************************************
  */

#ifndef _USB_CDC_H
#define _USB_CDC_H

/* 只为了拿到 ENABLE_USB_CDC / CFG_TUD_CDC 这几个宏。
 * tusb_config.h 本身是纯宏文件，不含任何 include，单独引它没有副作用。 */
#include <tusb_config.h>

#include <stdint.h>

#if ENABLE_USB_CDC

/* 初始化 USB 时钟、PA11/PA12、中断优先级并启动 TinyUSB。
 * 必须在 UsbAudio_PreInit() 之后调用（那个函数先把 D+ 拉低，
 * 这里再切换成 USB 外设复用功能，主机才会看到一次“插入”）。 */
void UsbCdc_Init(void);

/* 主循环任务：跑 TinyUSB 状态机 + 把发送 FIFO 推出去。
 * 每轮都要调，不能加延时，否则枚举会超时。 */
void UsbCdc_Task(void);

/* 非阻塞写。主机没连上、或 FIFO 满了，就直接返回实际写入的字节数（可能是 0），
 * 绝不等待——调试日志丢几个字节比卡住蓝牙时序好得多。 */
uint16_t UsbCdc_Write(const uint8_t *data, uint16_t len);

/* 非阻塞写字符串（给 UART1_SendString 做镜像用） */
void UsbCdc_SendString(const char *s);

/* 1 = 主机已打开串口（收到过 SET_CONTROL_LINE_STATE 的 DTR） */
uint8_t UsbCdc_IsReady(void);

#endif /* ENABLE_USB_CDC */

#endif /* _USB_CDC_H */

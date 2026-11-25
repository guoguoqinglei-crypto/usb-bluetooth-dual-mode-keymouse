#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU                OPT_MCU_STM32F1

#ifndef STM32F103xE
#define STM32F103xE
#endif

/* TinyUSB 不启用内置 RTOS 抽象，由 FreeRTOS 的 USB 任务周期推进协议栈 */
#define CFG_TUSB_OS                 OPT_OS_NONE

#define CFG_TUSB_DEBUG              0

/* 只有一个 USB 控制器，工作在设备模式、全速 */
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* 显式打开设备协议栈 */
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

/* 端点 0 最大包长：全速设备固定 64 字节 */
#define CFG_TUD_ENDPOINT0_SIZE      64

#define ENABLE_USB_CDC              0
#define CFG_TUD_CDC                 0

#define CFG_TUD_AUDIO               0

#define CFG_TUD_HID                 3

#define CFG_TUD_HID_EP_BUFSIZE      64

/* STM32F103 的 USB 寄存器基地址与 PMA（Packet Memory Area）基地址。
 * 标准外设库的 stm32f10x.h 没有导出这两个宏，这里显式给出。 */
#define FSDEV_REG_BASE              0x40005C00UL
#define FSDEV_PMA_BASE              0x40006000UL

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */

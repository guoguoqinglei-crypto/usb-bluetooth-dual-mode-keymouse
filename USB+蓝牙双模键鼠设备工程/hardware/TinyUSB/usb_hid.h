#ifndef USB_HID_H_
#define USB_HID_H_

#include <stdint.h>

#include <tusb_config.h>

#define HID_DESC_LEN_KEYBOARD       65
#define HID_DESC_LEN_MOUSE          77
#define HID_DESC_LEN_CUSTOM         27

/* 输入报告 9 字节：rep[8] 为 L3/R3 状态；输出报告仍为 8 字节。 */
#define USB_HID_CUSTOM_REPORT_LEN   9

/* 自定义 HID 输出报告的命令字 */
#define USB_HID_CMD_SET_RGB         0x01
#define USB_HID_CMD_GET_STATUS      0x02
#define USB_HID_CMD_RAINBOW_START   0x03
#define USB_HID_CMD_RAINBOW_STOP    0x04

/* 三个 HID 实例编号，与 usb_descriptors.h 里的接口顺序一致 */
#define USB_HID_INST_KEYBOARD       0
#define USB_HID_INST_MOUSE          1
#define USB_HID_INST_CUSTOM         2

/* 毫秒时基（TIM4 中断累加），TinyUSB 内部与主循环共用 */
uint32_t hal_time_ms(void);

/* 上电先把 D+ 拉低：外设没初始化完之前，主机不会开始枚举 */
void UsbHid_PreInit(void);

/* 启动 1ms 时基（幂等，可重复调用） */
void UsbHid_TimeBaseInit(void);

/* 配置 USB 并启动设备协议栈 */
void UsbHid_Init(void);

/* 主循环任务：内含 tud_task()，不能阻塞 */
void UsbHid_Task(void);

/* USB 已挂载（枚举完成）返回 1；蓝牙只据此选路，不仅凭 VBus 判断 */
uint8_t UsbHid_IsMounted(void);

/* USB / 蓝牙共用锁定状态，使两条链路的上位机都读到一致的 Caps / Num */
void UsbHid_SetKeyboardLeds(uint8_t leds);
uint8_t UsbHid_GetKeyboardLeds(void);

/* USB / 蓝牙共用板端彩虹开关 */
void UsbHid_SetRainbowMode(uint8_t on);

/* 长按 RG 键 2 秒可在"自动(USB 优先)"与"强制蓝牙"之间切换。
 * 返回 1 表示当前被强制为蓝牙模式：即使 USB 已挂载也不发 USB HID 报告。 */
uint8_t UsbHid_IsBtForced(void);

/* 用户是否把模式设成了"强制蓝牙"（掉电保存的偏好）。
 * 只要偏好是蓝牙，蓝牙任务就会继续跑来回连，即使当前临时回落到 USB。 */
uint8_t UsbHid_IsBtPreferred(void);

#endif /* USB_HID_H_ */

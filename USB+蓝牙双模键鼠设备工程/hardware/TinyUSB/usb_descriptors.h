#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>

#include <tusb_config.h>

enum
{
    ITF_NUM_HID_KEYBOARD = 0,       /* 键盘 */
    ITF_NUM_HID_MOUSE,              /* 鼠标 */
    ITF_NUM_HID_CUSTOM,             /* 自定义 HID */
    ITF_NUM_TOTAL
};

/* 键盘：中断 IN，端点 1 */
#define EPNUM_HID_KBD_IN        0x81

/* 鼠标：中断 IN，端点 2 */
#define EPNUM_HID_MOUSE_IN      0x82

/* 自定义 HID：中断 IN / OUT，端点 3 */
#define EPNUM_HID_CUSTOM_IN     0x83
#define EPNUM_HID_CUSTOM_OUT    0x03

#endif /* USB_DESCRIPTORS_H_ */

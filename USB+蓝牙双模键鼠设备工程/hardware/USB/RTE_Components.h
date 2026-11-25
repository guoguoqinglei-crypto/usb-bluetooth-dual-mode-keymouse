#ifndef RTE_COMPONENTS_H
#define RTE_COMPONENTS_H

#define RTE_Drivers_USBD0
#define RTE_USB_Core
#define RTE_USB_Device_0
#define RTE_USB_Device_HID_0

#define RTE_USB_DEVICE                 1
#define RTE_USB_DEVICE_CON_PIN         0

// HID 使用 usb_gamepad.c 中的自定义报告描述符
#define USBD_HID0_USER_REPORT_DESCRIPTOR       1
#define USBD_HID0_USER_REPORT_DESCRIPTOR_SIZE  45

#endif

#include <string.h>

#include <tusb.h>
#include <usb_descriptors.h>
#include <usb_hid.h>

#define USB_VID             0xCafe
#define USB_PID             0x4004
#define USB_BCD             0x0200

static const tusb_desc_device_t desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    /* 复合设备：设备级 Class/SubClass/Protocol 全部填 0，
     * 具体类信息写在各接口描述符里（HID = 0x03）。 */
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

#define CONFIG_TOTAL_LEN    91
#define CONFIG_NUM_ITF      3

static uint8_t const desc_configuration[] =
{
    0x09, 0x02, (uint8_t)(CONFIG_TOTAL_LEN & 0xFF), (uint8_t)(CONFIG_TOTAL_LEN >> 8),
    CONFIG_NUM_ITF, 0x01, 0x00, 0x80, 0x32,

    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x04,   /* iInterface=4：MP-KM Keyboard */

    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (uint8_t)(HID_DESC_LEN_KEYBOARD & 0xFF), (uint8_t)(HID_DESC_LEN_KEYBOARD >> 8),

    0x07, 0x05, EPNUM_HID_KBD_IN, 0x03, 0x40, 0x00, 0x01,

    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x05,   /* iInterface=5：MP-KM Mouse */

    /* HID 描述符（9 字节），报告描述符 77 字节 */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (uint8_t)(HID_DESC_LEN_MOUSE & 0xFF), (uint8_t)(HID_DESC_LEN_MOUSE >> 8),

    /* 中断 IN 端点（7 字节）：地址 0x82（端点 2，IN） */
    0x07, 0x05, EPNUM_HID_MOUSE_IN, 0x03, 0x40, 0x00, 0x01,

    0x09, 0x04, 0x02, 0x00, 0x02, 0x03, 0x00, 0x00, 0x06,   /* iInterface=6：MP-KM Custom */

    /* HID 描述符（9 字节），报告描述符 27 字节 */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (uint8_t)(HID_DESC_LEN_CUSTOM & 0xFF), (uint8_t)(HID_DESC_LEN_CUSTOM >> 8),

    0x07, 0x05, EPNUM_HID_CUSTOM_IN, 0x03, 0x40, 0x00, 0x01,

    /* 中断 OUT 端点（7 字节）：地址 0x03（端点 3，OUT）——上位机 -> 板子
     * 少了这一段，上位机写 RGB 命令会直接失败（Windows 只认中断 OUT）。 */
    0x07, 0x05, EPNUM_HID_CUSTOM_OUT, 0x03, 0x40, 0x00, 0x01
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "configuration descriptor length mismatch");

static const char *const string_desc_arr[] =
{
    (const char[]){0x09, 0x04},     /* 0: 语言 ID（英文 - 美国） */
    "Minzhan Training",             /* 1: 厂商 */
    "MP KEYMOUSE USB",             /* 2: 产品名（仅英文：回调按字节转 UTF-16） */
    "000001",                       /* 3: 序列号 */
    "MP-KM Keyboard",               /* 4: 接口 0（键盘）的名字 */
    "MP-KM Mouse",                  /* 5: 接口 1（鼠标）的名字 */
    "MP-KM Custom",                 /* 6: 接口 2（自定义 HID）的名字 */
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    static uint16_t desc_str[32];
    uint8_t chr_count;

    if (index == 0)
    {
        desc_str[1] = 0x0409;
        chr_count = 1;
    }
    else
    {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
        {
            return NULL;
        }

        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31)
        {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++)
        {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * chr_count + 2U));
    return desc_str;
}

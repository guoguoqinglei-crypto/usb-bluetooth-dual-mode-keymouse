#include <usb_gamepad.h>

#include <stm32f10x.h>

/* USB 外设寄存器基地址 */
#define USB_REG_BASE       0x40005C00UL
/* USB 专用 Packet Memory Area（PMA）基地址 */
#define USB_PMA_BASE       0x40006000UL

/* USB 公共寄存器 */
#define USB_CNTR           (*(volatile uint32_t *)(USB_REG_BASE + 0x40U))
#define USB_ISTR           (*(volatile uint32_t *)(USB_REG_BASE + 0x44U))
#define USB_FNR            (*(volatile uint32_t *)(USB_REG_BASE + 0x48U))
#define USB_DADDR          (*(volatile uint32_t *)(USB_REG_BASE + 0x4CU))
#define USB_BTABLE         (*(volatile uint32_t *)(USB_REG_BASE + 0x50U))

/* USB 端点寄存器，n 取值 0~7 */
#define USB_EPR(n)         (*(volatile uint32_t *)(USB_REG_BASE + 4U * (n)))

/* CNTR 控制位 */
/* EPnR 端点控制位 */
#define USB_EP_CTR_RX      0x8000U
#define USB_EP_DTOG_RX     0x4000U
#define USB_EP_STAT_RX     0x3000U
#define USB_EP_SETUP       0x0800U
#define USB_EP_TYPE        0x0600U
#define USB_EP_KIND        0x0100U
#define USB_EP_CTR_TX      0x0080U
#define USB_EP_DTOG_TX     0x0040U
#define USB_EP_STAT_TX     0x0030U
#define USB_EP_EA          0x000FU

#define USB_EP_TYPE_CONTROL    0x0200U
#define USB_EP_TYPE_INTERRUPT  0x0600U

#define USB_EP_TX_DIS      0x0000U
#define USB_EP_TX_STALL    0x0010U
#define USB_EP_TX_NAK      0x0020U
#define USB_EP_TX_VALID    0x0030U

#define USB_EP_RX_DIS      0x0000U
#define USB_EP_RX_STALL    0x1000U
#define USB_EP_RX_NAK      0x2000U
#define USB_EP_RX_VALID    0x3000U

/* 写 EPnR 时需要保留的可写位 */
#define USB_EP_WRITE_MASK  (USB_EP_CTR_RX | USB_EP_SETUP | USB_EP_TYPE | \
                            USB_EP_KIND | USB_EP_CTR_TX | USB_EP_EA)

/* PMA 中端点缓冲描述表，每项占 4 个 16 位单元 */
#define USB_EP0_TX_WORD    0x40U
#define USB_EP0_RX_WORD    0x80U
#define USB_EP1_TX_WORD    0xC0U

/* 控制传输阶段 */
#define EP0_PHASE_IDLE        0U
#define EP0_PHASE_DATA_IN     1U
#define EP0_PHASE_DATA_IN_ZLP 2U
#define EP0_PHASE_STATUS_OUT  3U
#define EP0_PHASE_STATUS_IN   4U

/* USB 标准请求 */
#define USB_REQ_GET_STATUS        0x00U
#define USB_REQ_CLEAR_FEATURE     0x01U
#define USB_REQ_SET_FEATURE       0x03U
#define USB_REQ_SET_ADDRESS       0x05U
#define USB_REQ_GET_DESCRIPTOR    0x06U
#define USB_REQ_SET_DESCRIPTOR    0x07U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define USB_REQ_GET_INTERFACE     0x0AU
#define USB_REQ_SET_INTERFACE     0x0BU

/* HID 类请求 */
#define HID_REQ_GET_REPORT        0x01U
#define HID_REQ_GET_IDLE          0x02U
#define HID_REQ_GET_PROTOCOL      0x03U
#define HID_REQ_SET_REPORT        0x09U
#define HID_REQ_SET_IDLE          0x0AU
#define HID_REQ_SET_PROTOCOL      0x0BU

/* USB 描述符类型 */
#define USB_DESC_DEVICE           0x01U
#define USB_DESC_CONFIGURATION    0x02U
#define USB_DESC_STRING           0x03U
#define USB_DESC_HID_REPORT       0x22U

/* HID 游戏手柄报告描述符：4 个 16 位轴 + 16 个按键，共 45 字节 */
static const uint8_t usbHidReportDescriptor[] =
{
    0x05, 0x01,        /* Usage Page: Generic Desktop */
    0x09, 0x05,        /* Usage: Game Pad */
    0xA1, 0x01,        /* Collection: Application */

    0x05, 0x01,        /* Usage Page: Generic Desktop */
    0x09, 0x30,        /* Usage: X */
    0x09, 0x31,        /* Usage: Y */
    0x09, 0x33,        /* Usage: Rx */
    0x09, 0x34,        /* Usage: Ry */
    0x16, 0x00, 0x80,  /* Logical Minimum: -32768 */
    0x26, 0xFF, 0x7F,  /* Logical Maximum: 32767 */
    0x75, 0x10,        /* Report Size: 16 */
    0x95, 0x04,        /* Report Count: 4 */
    0x81, 0x02,        /* Input: Data, Variable, Absolute */

    0x05, 0x09,        /* Usage Page: Buttons */
    0x19, 0x01,        /* Usage Minimum: Button 1 */
    0x29, 0x10,        /* Usage Maximum: Button 16 */
    0x15, 0x00,        /* Logical Minimum: 0 */
    0x25, 0x01,        /* Logical Maximum: 1 */
    0x75, 0x01,        /* Report Size: 1 */
    0x95, 0x10,        /* Report Count: 16 */
    0x81, 0x02,        /* Input: Data, Variable, Absolute */

    0xC0               /* End Collection */
};

/* USB 设备描述符，VID/PID 使用 ST 示例范围，Windows 无需额外驱动 */
static const uint8_t usbDeviceDescriptor[] =
{
    0x12, 0x01,        /* bLength, DEVICE */
    0x10, 0x01,        /* bcdUSB = USB 1.10 */
    0x00, 0x00, 0x00,  /* 类、子类、协议 */
    0x40,              /* EP0 最大包长 64 字节 */
    0x83, 0x04,        /* VID = 0x0483 */
    0x10, 0x57,        /* PID = 0x5710 */
    0x00, 0x01,        /* 设备版本 1.00 */
    0x01, 0x02, 0x00,  /* 厂商字符串、产品字符串、无序列号 */
    0x01               /* 配置数量 */
};

/* 配置描述符：1 个 HID 接口 + 1 个 10 字节中断输入端点 */
static const uint8_t usbConfigDescriptor[] =
{
    /* 配置描述符 */
    0x09, 0x02, 34, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,

    /* 接口描述符：HID 游戏手柄 */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,

    /* HID 描述符，报告描述符长度 45 字节 */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 45, 0x00,

    /* 中断输入端点 EP1 IN，每 5ms 轮询一次 */
    0x07, 0x05, 0x81, 0x03, 0x0A, 0x00, 0x05
};

/* 字符串描述符索引 0：语言 ID */
static const uint8_t usbStringLangId[] =
{
    0x04, 0x03, 0x09, 0x04
};

/* 字符串描述符索引 1：厂商 */
static const uint8_t usbStringManufacturer[] =
{
    0x0E, 0x03,
    'G', 0, 'T', 0, 'R', 0, '-', 0, '9', 0, '0', 0
};

/* 字符串描述符索引 2：产品名 */
static const uint8_t usbStringProduct[] =
{
    0x28, 0x03,
    'C', 0, 'F', 0, 'M', 0, 'O', 0, 'T', 0, 'O', 0,
    '-', 0, '0', 0, '0', 0, '4', 0, '9', 0, ' ', 0,
    'G', 0, 'a', 0, 'm', 0, 'e', 0, 'p', 0, 'a', 0, 'd', 0
};

/* 当前 HID 输入报告：4 个轴各 2 字节，按键位图 2 字节 */
static uint8_t gamepadReport[10];

/* 控制端点发送状态 */
static const uint8_t *ep0TxData;
static uint16_t ep0TxOffset;
static uint16_t ep0TxLength;
static uint16_t ep0RequestedLength;
static uint8_t ep0Phase;
static uint8_t ep0PendingAddress;

/* USB 枚举状态 */
static volatile uint8_t usbConfigured;
static volatile uint8_t ep1InBusy;

static void Usb_ResetEndpoints(void);
static void Usb_HandleSetupPacket(void);
static void Usb_HandleStandardRequest(const uint8_t *setup);
static void Usb_HandleHidRequest(const uint8_t *setup);
static void Usb_Ep0SendData(const uint8_t *data, uint16_t length,
                            uint16_t requestedLength);
static void Usb_Ep0SendNextPacket(void);
static void Usb_Ep0SendStatusIn(void);
static void Usb_Ep0Stall(void);
static void Usb_ConfigureEp1In(void);
void USB_LP_CAN1_RX0_IRQHandler(void);

/* 第 1 步：写入 PMA；每个 16 位数据占用 4 字节地址空间 */
static void Usb_PmaWriteWord(uint16_t wordIndex, uint16_t value)
{
    *(volatile uint16_t *)(USB_PMA_BASE + 4U * wordIndex) = value;
}

/* 第 2 步：从 USB 专用 PMA 读取一个 16 位字 */
static uint16_t Usb_PmaReadWord(uint16_t wordIndex)
{
    return *(volatile uint16_t *)(USB_PMA_BASE + 4U * wordIndex);
}

/* 第 3 步：把用户缓冲区复制到 PMA，按小端格式逐字节装入 */
static void Usb_CopyToPma(uint16_t wordIndex, const uint8_t *buffer,
                          uint16_t length)
{
    uint16_t i;
    uint16_t value;

    for (i = 0; i < length; i += 2U)
    {
        value = buffer[i];
        if ((i + 1U) < length)
        {
            value |= (uint16_t)((uint16_t)buffer[i + 1U] << 8);
        }
        Usb_PmaWriteWord(wordIndex + (i / 2U), value);
    }
}

/* 第 4 步：从 PMA 读取数据到用户缓冲区 */
static void Usb_CopyFromPma(uint8_t *buffer, uint16_t wordIndex,
                            uint16_t length)
{
    uint16_t i;
    uint16_t value;

    for (i = 0; i < length; i += 2U)
    {
        value = Usb_PmaReadWord(wordIndex + (i / 2U));
        buffer[i] = (uint8_t)(value & 0xFFU);
        if ((i + 1U) < length)
        {
            buffer[i + 1U] = (uint8_t)(value >> 8);
        }
    }
}

/* 第 5 步：修改端点发送状态，EPnR 的 STAT 位需要用异或方式写入 */
static void Usb_EpSetTxStatus(uint8_t ep, uint16_t status)
{
    uint32_t value = USB_EPR(ep);

    USB_EPR(ep) = (value ^ (status & USB_EP_STAT_TX)) &
                  (USB_EP_WRITE_MASK | USB_EP_STAT_TX);
}

/* 第 6 步：修改端点接收状态 */
static void Usb_EpSetRxStatus(uint8_t ep, uint16_t status)
{
    uint32_t value = USB_EPR(ep);

    USB_EPR(ep) = (value ^ (status & USB_EP_STAT_RX)) &
                  (USB_EP_WRITE_MASK | USB_EP_STAT_RX);
}

/* 第 7 步：清除端点收包完成标志 */
static void Usb_EpClearRxCtr(uint8_t ep)
{
    USB_EPR(ep) = USB_EPR(ep) & (~USB_EP_CTR_RX) & USB_EP_WRITE_MASK;
}

/* 第 8 步：清除端点发包完成标志 */
static void Usb_EpClearTxCtr(uint8_t ep)
{
    USB_EPR(ep) = USB_EPR(ep) & (~USB_EP_CTR_TX) & USB_EP_WRITE_MASK;
}

/* 第 9 步：设置端点发送包长度 */
static void Usb_SetEpTxCount(uint8_t ep, uint16_t length)
{
    Usb_PmaWriteWord((uint16_t)(ep * 4U + 1U), length);
}

/* 第 11 步：复位并配置 USB 端点，处理主机发出的 USB 复位 */
static void Usb_ResetEndpoints(void)
{
    uint32_t i;

    /* 第 11.1 步：停止当前端点活动，并清除所有 USB 中断标志 */
    USB_CNTR = 0;
    USB_ISTR = 0;
    USB_BTABLE = 0;

    /* 第 11.2 步：初始化端点缓冲区描述表 */
    Usb_PmaWriteWord(0U, USB_EP0_TX_WORD);
    Usb_PmaWriteWord(1U, 0U);
    Usb_PmaWriteWord(2U, USB_EP0_RX_WORD);
    Usb_PmaWriteWord(3U, 0x8020U);       /* EP0 接收 64 字节缓冲 */

    Usb_PmaWriteWord(4U, USB_EP1_TX_WORD);
    Usb_PmaWriteWord(5U, 0U);
    Usb_PmaWriteWord(6U, 0U);
    Usb_PmaWriteWord(7U, 0U);

    /* 第 11.3 步：配置控制端点 EP0 */
    USB_EPR(0) = USB_EP_TYPE_CONTROL | 0U;
    Usb_EpSetTxStatus(0U, USB_EP_TX_NAK);
    Usb_EpSetRxStatus(0U, USB_EP_RX_VALID);

    /* 第 11.4 步：暂时关闭 EP1，等主机 SET_CONFIGURATION 后再打开 */
    USB_EPR(1) = USB_EP_TYPE_INTERRUPT | 1U;
    Usb_EpSetTxStatus(1U, USB_EP_TX_NAK);
    Usb_EpSetRxStatus(1U, USB_EP_RX_DIS);

    /* 第 11.5 步：复位后地址归零，并等待主机重新分配地址 */
    USB_DADDR = 0x80U;

    /* 第 11.6 步：复位软件状态 */
    ep0Phase = EP0_PHASE_IDLE;
    ep0PendingAddress = 0U;
    usbConfigured = 0U;
    ep1InBusy = 0U;

    /* 第 11.7 步：只打开需要的中断，避免不必要地占用 CPU */
    USB_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SUSPM |
               USB_CNTR_WKUPM | USB_CNTR_ERRM | USB_CNTR_PMAOVRM;

    /* 第 11.8 步：等待寄存器写入完成 */
    for (i = 0U; i < 1000U; i++)
    {
        __NOP();
    }
}

/* 第 12 步：让 EP1 IN 端点进入可发送状态 */
static void Usb_ConfigureEp1In(void)
{
    USB_EPR(1) = USB_EP_TYPE_INTERRUPT | 1U;
    Usb_EpSetTxStatus(1U, USB_EP_TX_NAK);
    Usb_EpSetRxStatus(1U, USB_EP_RX_DIS);
    ep1InBusy = 0U;
}

/* 第 13 步：控制端点在发现不支持的请求时返回 STALL */
static void Usb_Ep0Stall(void)
{
    ep0Phase = EP0_PHASE_IDLE;
    Usb_EpSetTxStatus(0U, USB_EP_TX_STALL);
    Usb_EpSetRxStatus(0U, USB_EP_RX_STALL);
}

/* 第 14 步：发送控制传输的 0 字节状态包 */
static void Usb_Ep0SendStatusIn(void)
{
    Usb_SetEpTxCount(0U, 0U);
    ep0Phase = EP0_PHASE_STATUS_IN;
    Usb_EpSetRxStatus(0U, USB_EP_RX_NAK);
    Usb_EpSetTxStatus(0U, USB_EP_TX_VALID);
}

/* 第 15 步：发送控制传输数据阶段的下一包 */
static void Usb_Ep0SendNextPacket(void)
{
    uint16_t remaining = (uint16_t)(ep0TxLength - ep0TxOffset);
    uint16_t chunk = (remaining > 64U) ? 64U : remaining;

    /* 第 16.1 步：把当前数据包写入 EP0 PMA */
    Usb_CopyToPma(USB_EP0_TX_WORD, ep0TxData + ep0TxOffset, chunk);
    Usb_SetEpTxCount(0U, chunk);
    ep0TxOffset = (uint16_t)(ep0TxOffset + chunk);

    /* 第 16.2 步：判断是否还要续传或发送结尾零长度包 */
    if (ep0TxOffset < ep0TxLength)
    {
        ep0Phase = EP0_PHASE_DATA_IN;
    }
    else if ((ep0TxLength == 0U) ||
             ((chunk == 64U) && (ep0RequestedLength > ep0TxLength)))
    {
        ep0Phase = EP0_PHASE_DATA_IN_ZLP;
    }
    else
    {
        ep0Phase = EP0_PHASE_STATUS_OUT;
    }

    /* 第 16.3 步：数据阶段不接收 OUT 数据 */
    Usb_EpSetRxStatus(0U, USB_EP_RX_NAK);
    Usb_EpSetTxStatus(0U, USB_EP_TX_VALID);
}

/* 第 16 步：开始一次控制输入传输 */
static void Usb_Ep0SendData(const uint8_t *data, uint16_t length,
                            uint16_t requestedLength)
{
    if (requestedLength < length)
    {
        length = requestedLength;
    }

    ep0TxData = data;
    ep0TxOffset = 0U;
    ep0TxLength = length;
    ep0RequestedLength = requestedLength;

    /* 第 17.1 步：长度为 0 时直接返回零长度状态包 */
    if (length == 0U)
    {
        Usb_Ep0SendStatusIn();
        return;
    }

    /* 第 17.2 步：发送第一包数据 */
    Usb_Ep0SendNextPacket();
}

/* 第 17 步：解析设备请求，并准备对应的描述符或状态包 */
static void Usb_HandleStandardRequest(const uint8_t *setup)
{
    uint16_t value = (uint16_t)((uint16_t)setup[2] |
                                ((uint16_t)setup[3] << 8));
    uint16_t length = (uint16_t)((uint16_t)setup[6] |
                                 ((uint16_t)setup[7] << 8));
    uint8_t request = setup[1];
    uint8_t descriptorType;
    uint8_t descriptorIndex;

    /* 第 18.1 步：GET_STATUS 返回 2 字节状态 */
    if (request == USB_REQ_GET_STATUS)
    {
        static const uint8_t status[2] = {0x00U, 0x00U};
        Usb_Ep0SendData(status, 2U, length);
        return;
    }

    /* 第 18.2 步：设置地址请求在状态包完成后才生效 */
    if (request == USB_REQ_SET_ADDRESS)
    {
        ep0PendingAddress = (uint8_t)(value & 0x7FU);
        Usb_Ep0SendStatusIn();
        return;
    }

    /* 第 18.3 步：主机的 GET_DESCRIPTOR 请求 */
    if (request == USB_REQ_GET_DESCRIPTOR)
    {
        descriptorType = (uint8_t)(value >> 8);
        descriptorIndex = (uint8_t)(value & 0xFFU);

        if (descriptorType == USB_DESC_DEVICE)
        {
            Usb_Ep0SendData(usbDeviceDescriptor,
                            (uint16_t)sizeof(usbDeviceDescriptor), length);
            return;
        }

        if (descriptorType == USB_DESC_CONFIGURATION)
        {
            Usb_Ep0SendData(usbConfigDescriptor,
                            (uint16_t)sizeof(usbConfigDescriptor), length);
            return;
        }

        if (descriptorType == USB_DESC_STRING)
        {
            if (descriptorIndex == 0U)
            {
                Usb_Ep0SendData(usbStringLangId,
                                (uint16_t)sizeof(usbStringLangId), length);
            }
            else if (descriptorIndex == 1U)
            {
                Usb_Ep0SendData(usbStringManufacturer,
                                (uint16_t)sizeof(usbStringManufacturer),
                                length);
            }
            else if (descriptorIndex == 2U)
            {
                Usb_Ep0SendData(usbStringProduct,
                                (uint16_t)sizeof(usbStringProduct), length);
            }
            else
            {
                Usb_Ep0Stall();
            }
            return;
        }

        if (descriptorType == USB_DESC_HID_REPORT)
        {
            Usb_Ep0SendData(usbHidReportDescriptor,
                            (uint16_t)sizeof(usbHidReportDescriptor), length);
            return;
        }

        Usb_Ep0Stall();
        return;
    }

    /* 第 18.4 步：设置配置值，并在配置 1 时打开 HID 端点 */
    if (request == USB_REQ_SET_CONFIGURATION)
    {
        if ((value & 0xFFU) != 0U)
        {
            Usb_ConfigureEp1In();
            usbConfigured = 1U;
        }
        else
        {
            Usb_EpSetTxStatus(1U, USB_EP_TX_NAK);
            Usb_EpSetRxStatus(1U, USB_EP_RX_DIS);
            usbConfigured = 0U;
        }
        Usb_Ep0SendStatusIn();
        return;
    }

    /* 第 18.5 步：返回当前配置值 */
    if (request == USB_REQ_GET_CONFIGURATION)
    {
        static const uint8_t configuration = 0x01U;
        Usb_Ep0SendData(&configuration, 1U, length);
        return;
    }

    /* 第 18.6 步：HID 接口只有一个备用设置，固定返回 0 */
    if (request == USB_REQ_GET_INTERFACE)
    {
        static const uint8_t interface = 0x00U;
        Usb_Ep0SendData(&interface, 1U, length);
        return;
    }

    /* 第 18.7 步：这些请求没有数据阶段，只返回成功状态 */
    if ((request == USB_REQ_SET_INTERFACE) ||
        (request == USB_REQ_CLEAR_FEATURE) ||
        (request == USB_REQ_SET_FEATURE) ||
        (request == USB_REQ_SET_DESCRIPTOR))
    {
        Usb_Ep0SendStatusIn();
        return;
    }

    /* 第 18.8 步：未知标准请求返回 STALL */
    Usb_Ep0Stall();
}

/* 第 18 步：解析 HID 类请求 */
static void Usb_HandleHidRequest(const uint8_t *setup)
{
    uint16_t length = (uint16_t)((uint16_t)setup[6] |
                                 ((uint16_t)setup[7] << 8));
    uint8_t request = setup[1];

    /* 第 19.1 步：GET_REPORT 返回当前手柄报告 */
    if (request == HID_REQ_GET_REPORT)
    {
        Usb_Ep0SendData((const uint8_t *)gamepadReport, 10U, length);
        return;
    }

    /* 第 19.2 步：报告协议固定返回 1，IDLE 空闲速率返回 0 */
    if (request == HID_REQ_GET_PROTOCOL)
    {
        static const uint8_t protocol = 0x01U;
        Usb_Ep0SendData(&protocol, 1U, length);
        return;
    }

    if (request == HID_REQ_GET_IDLE)
    {
        static const uint8_t idle = 0x00U;
        Usb_Ep0SendData(&idle, 1U, length);
        return;
    }

    /* 第 19.3 步：HID 设置请求没有返回数据，只发送成功状态 */
    if ((request == HID_REQ_SET_REPORT) ||
        (request == HID_REQ_SET_IDLE) ||
        (request == HID_REQ_SET_PROTOCOL))
    {
        Usb_Ep0SendStatusIn();
        return;
    }

    /* 第 19.4 步：未知 HID 请求返回 STALL */
    Usb_Ep0Stall();
}

/* 第 19 步：处理主机发来的 8 字节 SETUP 包 */
static void Usb_HandleSetupPacket(void)
{
    uint8_t setup[8];
    uint8_t requestType;

    /* 第 20.1 步：从 EP0 接收缓冲区读取 SETUP 包 */
    Usb_CopyFromPma(setup, USB_EP0_RX_WORD, 8U);

    /* 第 20.2 步：处理新的 SETUP 前，先让 EP0 保持 NAK */
    Usb_EpSetTxStatus(0U, USB_EP_TX_NAK);
    Usb_EpSetRxStatus(0U, USB_EP_RX_NAK);
    ep0Phase = EP0_PHASE_IDLE;

    requestType = setup[0];

    /* 第 20.3 步：标准请求 */
    if ((requestType & 0x60U) == 0x00U)
    {
        Usb_HandleStandardRequest(setup);
        return;
    }

    /* 第 20.4 步：HID 类请求 */
    if ((requestType & 0x60U) == 0x20U)
    {
        Usb_HandleHidRequest(setup);
        return;
    }

    /* 第 20.5 步：其他请求暂不支持 */
    Usb_Ep0Stall();
}

/* 第 20 步：初始化 STM32F103 原生 USB 外设 */
uint8_t UsbGamepad_Init(void)
{
    uint32_t gpioConfig;
    uint32_t i;

    /* 第 21.1 步：开启 GPIOA 和 USB 外设时钟 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USBEN;

    /* 第 21.2 步：72MHz 系统时钟必须选择 USB 1.5 分频，得到 48MHz */
    RCC->CFGR &= ~RCC_CFGR_USBPRE;

    /* 第 21.3 步：PA11/PA12 配置为 USB 复用开漏输出 */
    gpioConfig = GPIOA->CRH;
    gpioConfig &= ~((0x0FU << 12) | (0x0FU << 16));
    gpioConfig |= ((0x0BU << 12) | (0x0BU << 16));
    GPIOA->CRH = gpioConfig;

    /* 第 21.4 步：复位 USB 外设，确保从确定状态开始 */
    RCC->APB1RSTR |= RCC_APB1RSTR_USBRST;
    for (i = 0U; i < 1000U; i++)
    {
        __NOP();
    }
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USBRST;

    /* 第 21.5 步：先强制断开，再释放 USB 内部 D+ 上拉 */
    USB_CNTR = USB_CNTR_FRES;
    USB_CNTR = 0U;
    USB_ISTR = 0U;

    /* 第 21.6 步：建立端点 0 和端点 1 的初始状态 */
    Usb_ResetEndpoints();

    /* 第 21.7 步：打开 USB 低优先级中断 */
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 2U);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);

    return 0U;
}

/* 第 21 步：主机完成枚举后，把 4 轴和按键报告发送到 EP1 IN */
uint8_t UsbGamepad_Send(int16_t x, int16_t y, int16_t rx, int16_t ry,
                        uint16_t buttons)
{
    uint16_t ux;
    uint16_t uy;
    uint16_t urx;
    uint16_t ury;

    /* 第 21.1 步：进入临界区，避免主循环和 USB 中断同时修改端点 */
    __disable_irq();

    if ((usbConfigured == 0U) || (ep1InBusy != 0U))
    {
        __enable_irq();
        return 1U;
    }

    /* 第 21.2 步：把 16 位有符号数转换成小端字节序 */
    ux = (uint16_t)x;
    uy = (uint16_t)y;
    urx = (uint16_t)rx;
    ury = (uint16_t)ry;

    gamepadReport[0] = (uint8_t)(ux & 0xFFU);
    gamepadReport[1] = (uint8_t)(ux >> 8);
    gamepadReport[2] = (uint8_t)(uy & 0xFFU);
    gamepadReport[3] = (uint8_t)(uy >> 8);
    gamepadReport[4] = (uint8_t)(urx & 0xFFU);
    gamepadReport[5] = (uint8_t)(urx >> 8);
    gamepadReport[6] = (uint8_t)(ury & 0xFFU);
    gamepadReport[7] = (uint8_t)(ury >> 8);
    gamepadReport[8] = (uint8_t)(buttons & 0xFFU);
    gamepadReport[9] = (uint8_t)(buttons >> 8);

    /* 第 21.3 步：把报告写入 EP1 的 PMA 缓冲区 */
    Usb_CopyToPma(USB_EP1_TX_WORD, (const uint8_t *)gamepadReport, 10U);
    Usb_SetEpTxCount(1U, 10U);

    /* 第 21.4 步：标记忙，并让端点向主机发送数据 */
    ep1InBusy = 1U;
    Usb_EpSetTxStatus(1U, USB_EP_TX_VALID);

    /* 第 21.5 步：恢复中断 */
    __enable_irq();

    return 0U;
}

/* 第 22 步：返回主机是否已经完成 SET_CONFIGURATION */
uint8_t UsbGamepad_IsConfigured(void)
{
    return usbConfigured;
}

/* 第 23 步：USB 低优先级中断服务函数 */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    uint32_t status = USB_ISTR;

    /* 第 23.1 步：USB 复位中断 */
    if ((status & USB_ISTR_RESET) != 0U)
    {
        Usb_ResetEndpoints();
        USB_ISTR = (uint32_t)(~USB_ISTR_RESET);
    }

    /* 第 23.2 步：挂起、唤醒和错误中断只需要清除标志 */
    if ((status & USB_ISTR_SUSP) != 0U)
    {
        USB_ISTR = (uint32_t)(~USB_ISTR_SUSP);
    }

    if ((status & USB_ISTR_WKUP) != 0U)
    {
        USB_ISTR = (uint32_t)(~USB_ISTR_WKUP);
    }

    if ((status & USB_ISTR_ERR) != 0U)
    {
        USB_ISTR = (uint32_t)(~USB_ISTR_ERR);
    }

    if ((status & USB_ISTR_PMAOVR) != 0U)
    {
        USB_ISTR = (uint32_t)(~USB_ISTR_PMAOVR);
    }

    /* 第 23.3 步：处理正确传输中断 */
    if ((status & USB_ISTR_CTR) != 0U)
    {
        uint8_t ep = (uint8_t)(USB_ISTR & USB_ISTR_EP_ID);
        uint32_t epStatus = USB_EPR(ep);

        /* 第 23.4 步：控制端点 EP0 接收方向 */
        if ((ep == 0U) && ((epStatus & USB_EP_CTR_RX) != 0U))
        {
            Usb_EpClearRxCtr(0U);

            if ((epStatus & USB_EP_SETUP) != 0U)
            {
                Usb_HandleSetupPacket();
            }
            else
            {
                /* 第 23.5 步：OUT 状态包完成，控制传输结束 */
                ep0Phase = EP0_PHASE_IDLE;
                Usb_EpSetTxStatus(0U, USB_EP_TX_NAK);
                Usb_EpSetRxStatus(0U, USB_EP_RX_VALID);
            }
        }

        /* 第 23.6 步：控制端点 EP0 发送方向 */
        if ((ep == 0U) && ((epStatus & USB_EP_CTR_TX) != 0U))
        {
            Usb_EpClearTxCtr(0U);

            if (ep0Phase == EP0_PHASE_DATA_IN)
            {
                /* 第 23.7 步：继续发送下一条描述符数据 */
                Usb_Ep0SendNextPacket();
            }
            else if (ep0Phase == EP0_PHASE_DATA_IN_ZLP)
            {
                /* 第 23.8 步：发送零长度包，表示数据阶段结束 */
                Usb_SetEpTxCount(0U, 0U);
                ep0Phase = EP0_PHASE_STATUS_OUT;
                Usb_EpSetRxStatus(0U, USB_EP_RX_NAK);
                Usb_EpSetTxStatus(0U, USB_EP_TX_VALID);
            }
            else
            {
                /* 第 23.9 步：状态发送完成，等待主机的 OUT 状态包 */
                if (ep0Phase == EP0_PHASE_STATUS_IN)
                {
                    if (ep0PendingAddress != 0U)
                    {
                        USB_DADDR = 0x80U | ep0PendingAddress;
                        ep0PendingAddress = 0U;
                    }
                }

                ep0Phase = EP0_PHASE_IDLE;
                Usb_EpSetTxStatus(0U, USB_EP_TX_NAK);
                Usb_EpSetRxStatus(0U, USB_EP_RX_VALID);
            }
        }

        /* 第 23.10 步：EP1 报告发送完成，允许主循环发下一帧 */
        if ((ep == 1U) && ((epStatus & USB_EP_CTR_TX) != 0U))
        {
            Usb_EpClearTxCtr(1U);
            ep1InBusy = 0U;
        }
    }
}

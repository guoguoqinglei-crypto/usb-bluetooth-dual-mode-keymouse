/* ============================================================================
 * app_bt.c —— H4 传输层：字节流拆包、ACL 发送、链路自检
 * ----------------------------------------------------------------------------
 * 对应 ESP32_VHCI_demo/main/app_bt.c：
 *   demo 里 host_rcv_pkt() 由 VHCI 中断直接给一个完整 H4 包；
 *   STM32 这边 USART2 收到的是一串连续字节，所以先由 htc_parse_byte()
 *   拼包，凑满再交给 ap_app_receive() 按包类型分发。
 *
 * 本文件只干四件事：拆包、分发、ACL 分包发送、上电后的串口自检。
 * 命令怎么组装在 hci_command.c，事件怎么处理在 hci_event.c。
 * ========================================================================== */

#include "app_bt.h"

#include <string.h>
#include <stm32f10x.h>

#include <delay.h>
#include <bt_uart.h>
#include <uart.h>

/* 打印 HCI 收发明细：1=开（会明显拖慢主循环） 0=关 */
#define BT_TRACE  1

/* ---------------- 全局状态 ---------------- */
volatile uint16_t conn_handle      = BT_HANDLE_NONE;
uint8_t           peer_bdaddr[6]   = {0};
uint8_t           link_key[16]     = {0};
volatile uint8_t  link_key_valid   = 0U;
volatile uint16_t acl_mtu          = 128U;
volatile uint16_t acl_pkts         = 4U;
volatile uint32_t hci_cmd_count    = 0U;
volatile uint32_t hci_rx_events    = 0U;
volatile uint16_t hci_last_event   = 0xFFFFU;
volatile uint8_t  hci_last_status  = 0xFFU;

volatile uint8_t  bt_state   = 0U;
volatile uint32_t bt_reports = 0U;
volatile uint8_t  bt_err     = BT_STATE_OK;

/* ==========================================================================
 * 一、H4 拆包状态机
 * ========================================================================== */
#define BT_H4_BUF_SIZE  700U

static uint8_t  s_h4[BT_H4_BUF_SIZE];   /* 正在拼的包 */
static uint16_t s_h4Len;                /* 已收到字节数 */
static uint16_t s_h4Need;               /* 整包总长；0 = 还没凑够判断条件 */

void bt_transport_init(void)
{
    s_h4Len  = 0U;
    s_h4Need = 0U;
    BtUart_Flush();
}

/* 喂一个字节进去，凑满一包就分发出去 */
/* 一个完整的 H4 包到齐了：按包类型分发。
 * 对应 demo 的 ap_app_receive() —— 那边是 VHCI 回调直接把整包送进来，
 * 这边是拆包状态机凑满之后自己调。 */
static void ap_app_receive(uint8_t *data, uint16_t len)
{
    if (len < 2U)
    {
        return;
    }

    switch (data[0])
    {
        case H4_TYPE_EVENT:
            hci_last_event = (uint16_t)data[1];
            hci_rx_events++;
            hci_event_recv(&data[1], (uint16_t)(len - 1U));
            break;

        case H4_TYPE_ACL:
        {
            uint16_t flags = (uint16_t)(data[1] | ((uint16_t)data[2] << 8));
            uint8_t  pb    = (uint8_t)((flags >> 12) & 0x03U);
            LMacl_receive(pb, &data[4], (uint16_t)(len - 4U));
            break;
        }

        default:
            break;      /* 命令回显 / SCO 不当主机侧用，忽略 */
    }
}

static void hci_h4_parse_byte(uint8_t b)
{
    if (s_h4Len >= BT_H4_BUF_SIZE)
    {
        s_h4Len  = 0U;
        s_h4Need = 0U;
        return;
    }

    s_h4[s_h4Len] = b;
    s_h4Len++;

    /* 1. 还没算出整包长度：按包类型看头部 */
    if (s_h4Need == 0U)
    {
        switch (s_h4[0])
        {
            case H4_TYPE_CMD:
                if (s_h4Len >= 3U)
                {
                    s_h4Need = (uint16_t)(3U + s_h4[2]);
                }
                break;

            case H4_TYPE_EVENT:
                if (s_h4Len >= 2U)
                {
                    s_h4Need = (uint16_t)(2U + s_h4[1]);
                }
                break;

            case H4_TYPE_ACL:
                if (s_h4Len >= 4U)
                {
                    s_h4Need = (uint16_t)(4U + (uint16_t)(s_h4[2] | ((uint16_t)s_h4[3] << 8)));
                }
                break;

            case H4_TYPE_SCO:
                if (s_h4Len >= 3U)
                {
                    s_h4Need = (uint16_t)(3U + s_h4[2]);
                }
                break;

            default:
                /* 不是 H4 指示符（多半是波特率不对导致的错位），丢掉重来 */
                s_h4Len = 0U;
                return;
        }

        if (s_h4Need > BT_H4_BUF_SIZE)
        {
            s_h4Len  = 0U;
            s_h4Need = 0U;
            return;
        }
    }

    if ((s_h4Need == 0U) || (s_h4Len < s_h4Need))
    {
        return;
    }

    /* 2. 整包到齐，交给分发函数 */
    ap_app_receive(s_h4, s_h4Need);

    s_h4Len  = 0U;
    s_h4Need = 0U;
}

void bt_transport_poll(void)
{
    uint8_t  b;
    uint16_t guard = 0U;

    /* 一次最多取 256 个字节，避免控制器狂发时把主循环卡在这里：
     * 主循环还要采样摇杆、跑 USB，必须定期让出去。 */
    while ((guard < 256U) && (BtUart_TryRead(&b) != 0U))
    {
        hci_h4_parse_byte(b);
        guard++;
    }
}

static void bt_acl_write(uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len)
{
    uint8_t  hdr[5];
    uint16_t flags = (uint16_t)(handle & 0x0FFFU);

    flags |= (uint16_t)(((uint16_t)pb & 0x03U) << 12);

    hdr[0] = H4_TYPE_ACL;
    hdr[1] = (uint8_t)(flags & 0x00FFU);
    hdr[2] = (uint8_t)(flags >> 8);
    hdr[3] = (uint8_t)(len & 0x00FFU);
    hdr[4] = (uint8_t)(len >> 8);

    uart_send(hdr, 5U);
    if (len > 0U)
    {
        uart_send(data, len);
    }
}

/* 发一条 ACL 数据；超过控制器单包上限时自动分片（PB=0x02 首片 / 0x01 续片） */
uint8_t LMacl_send(uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len)
{
    uint16_t mtu = (acl_mtu >= 27U) ? acl_mtu : 128U;
    uint16_t off = 0U;
    uint8_t  first = 1U;
    uint16_t chunk;

    while (off < len)
    {
        chunk = (uint16_t)(len - off);
        if (chunk > mtu)
        {
            chunk = mtu;
        }

        bt_acl_write(handle, first ? pb : (uint8_t)0x01U, &data[off], chunk);

#if BT_TRACE
        UART1_Printf("TX acl %u/%u\r\n", (unsigned)chunk, (unsigned)len);
#endif

        off = (uint16_t)(off + chunk);
        first = 0U;

        /* 给控制器留出搬运时间，避免连续灌包把它灌丢 */
        if (off < len)
        {
            Delay_ms(8);
        }
    }

    return 1U;
}

/* ==========================================================================
 * 四、链路自检：芯片到底在哪组串口参数上应答
 * --------------------------------------------------------------------------
 * 补丁灌完、切到 921600 8E1 之后，如果 19 条命令一个事件都收不到，
 * 只会是三种情况之一：
 *   1. 芯片根本没跑起来（补丁没进去 / 没供电 / 没 32k 时钟）
 *   2. 串口参数不对（校验位、波特率跟芯片实际的不一致）
 *   3. 线不通（PA2/PA3 没接到模块，或被别的器件抢了）
 *
 * 这一函数负责把 1 和 2 分开：拿一条只读命令 Read_Local_Version
 * （不改芯片任何状态）在几组候选参数上各问一次，谁有回音就用谁，
 * 并把回音的原始字节打出来。返回 1 = 找到能通的那组，参数已设好；
 * 返回 0 = 四组全是死线，问题在硬件侧，不该再改软件。
 *
 * 只在开机跑一次，代价约 0.6 秒；确认硬件通了之后，
 * 把 main.c 里那次调用注释掉即可，不影响后面的流程。
 * ========================================================================== */
uint8_t bt_link_probe(void)
{
    typedef struct
    {
        uint32_t baud;
        uint32_t parity;       /* 用 uint32 存，结构体没有填充字节 */
    } bt_line_t;

    /* 顺序有讲究：先试厂商流程的正常值，成功就立刻返回，
     * 后面两组的波特率不对，发给芯片是乱码，尽量别走到 */
    static const bt_line_t lines[4] =
    {
        { 921600U, USART_Parity_Even },   /* 厂商流程：正常值 */
        { 921600U, USART_Parity_No   },   /* 有的固件切完是 8N1 */
        { 115200U, USART_Parity_No   },   /* 补丁没生效，芯片还在 ROM */
        { 115200U, USART_Parity_Even }
    };

    uint8_t  cap[32];
    uint8_t  n;
    uint8_t  i;
    uint8_t  found;
    uint16_t ms;

    for (i = 0U; i < 4U; i++)
    {
        BtUart_SetBaud(lines[i].baud, (uint16_t)lines[i].parity);  /* 内部会冲掉残留 */
        bt_transport_init();

        /* Read_Local_Version_Information：纯读取，不动芯片状态 */
        hci_send_cmd(HCI_OPCODE(OGF_INFO_PARAMS, 0x001), 0, 0U);

        /* 150ms 内把收到的字节原样抓下来（绕过 H4 拆包，先看有没有回音） */
        n = 0U;
        for (ms = 0U; ms < 150U; ms++)
        {
            uint8_t b;
            while ((n < (uint8_t)sizeof(cap)) && (BtUart_TryRead(&b) != 0U))
            {
                cap[n] = b;
                n++;
            }
            Delay_ms(1);
        }
        if (n == 0U)
        {
            while ((n < (uint8_t)sizeof(cap)) && (BtUart_TryRead(&cap[n]) != 0U))
            {
                n++;
            }
        }

        UART1_Printf("[bt] probe baud=%lu par=%c rx=%u :",
                     (unsigned long)lines[i].baud,
                     (lines[i].parity == USART_Parity_Even) ? 'E' : 'N',
                     (unsigned)n);
        for (ms = 0U; ms < (uint16_t)n; ms++)
        {
            UART1_Printf(" %02X", (unsigned)cap[ms]);
        }
        UART1_SendString("\r\n");

        /* 只有收到 Command_Complete（04 0E）才算真通：
         * 参数错时会收到一堆带奇偶/帧错误的乱码，那不算 */
        found = 0U;
        for (ms = 0U; (uint16_t)(ms + 1U) < (uint16_t)n; ms++)
        {
            if ((cap[ms] == H4_TYPE_EVENT) && (cap[ms + 1U] == EVT_Command_Complete))
            {
                found = 1U;
                break;
            }
        }

        if (found != 0U)
        {
            UART1_SendString("[bt] probe: link alive, keep this setting\r\n");
            bt_transport_init();
            return 1U;
        }
    }

    /* 四组都没回音：恢复厂商默认参数，让后面的流程按原样跑完 */
    BtUart_SetBaud(921600U, USART_Parity_Even);
    bt_transport_init();
    UART1_SendString("[bt] probe: DEAD on all 4 settings (hardware side)\r\n");
    return 0U;
}

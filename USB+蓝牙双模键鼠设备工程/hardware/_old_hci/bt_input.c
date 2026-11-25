/* ============================================================================
 * bt_input.c —— 手柄应用层：上电流程、HID 报告、Link Key 持久化、主动回连
 * ----------------------------------------------------------------------------
 * 这一层是 ESP32 demo 里没有的（demo 只到 SDP，不发输入报告），
 * 按讲师 75:58 的验收要求补上：
 *   "移植到 STM32，配对 + 回连，把两个摇杆和 8 个按键的数据传到电脑上"
 * ========================================================================== */

#include "app_bt.h"

#include <string.h>
#include <stm32f10x.h>
#include <stm32f10x_flash.h>

#include <delay.h>
#include <uart.h>
#include <joystick.h>
#include <key.h>
#include <usb_audio.h>          /* hal_time_ms()：TIM4 维护的 1ms 时基 */
#include <Init_rda5876.h>           /* rda_stage：初始化卡在第几步 */

/* ==========================================================================
 * 一、Link Key 持久化（回连的基础）
 * --------------------------------------------------------------------------
 * F103RCT6 是 256KB Flash，页大小 2KB，最后 2KB（0x0803F800）留给绑定信息。
 * 代码不到 130KB，不会碰到这里。
 * ========================================================================== */
#define BOND_FLASH_ADDR   0x0803F800U
#define BOND_MAGIC        0xB700B007U

static uint32_t s_bondWords[8];    /* 32 字节，4 字节对齐 */

static uint32_t bond_checksum(const uint8_t *b)
{
    uint32_t s = 0xA5A5A5A5U;
    uint8_t  i;
    for (i = 0U; i < 28U; i++)
    {
        s = (s ^ b[i]) + 0x9E3779B9U;
    }
    return s;
}

static void bond_load(void)
{
    const uint8_t *b = (const uint8_t *)BOND_FLASH_ADDR;
    uint32_t magic;
    uint32_t sum;

    memcpy(&magic, b, 4U);
    if (magic != BOND_MAGIC)
    {
        link_key_valid = 0U;
        return;
    }

    memcpy(&sum, &b[28], 4U);
    if (sum != bond_checksum(b))
    {
        link_key_valid = 0U;
        UART1_SendString("[bt] bond checksum bad, ignore\r\n");
        return;
    }

    memcpy((void *)peer_bdaddr, &b[4], 6U);
    memcpy((void *)link_key, &b[12], 16U);
    link_key_valid = 1U;
    UART1_SendString("[bt] bond loaded from flash\r\n");
}

void bt_input_save_bond(const uint8_t addr[6], const uint8_t key[16])
{
    uint8_t  *b = (uint8_t *)s_bondWords;
    uint8_t   i;
    uint32_t  magic = BOND_MAGIC;
    uint32_t  sum;

    memset(b, 0, 32U);
    memcpy(&b[0], &magic, 4U);
    memcpy(&b[4], addr, 6U);
    memcpy(&b[12], key, 16U);

    sum = bond_checksum(b);
    memcpy(&b[28], &sum, 4U);

    /* 内容没变就别擦页（擦写次数有限，而且擦页会短暂停住取指） */
    if (memcmp((const void *)BOND_FLASH_ADDR, b, 32U) == 0)
    {
        return;
    }

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(BOND_FLASH_ADDR);

    for (i = 0U; i < 8U; i++)
    {
        FLASH_ProgramWord(BOND_FLASH_ADDR + (uint32_t)i * 4U, s_bondWords[i]);
    }

    FLASH_Lock();

    UART1_SendString(memcmp((const void *)BOND_FLASH_ADDR, b, 32U) == 0
                     ? "[bt] bond saved\r\n" : "[bt] bond save FAIL\r\n");
}

/* ==========================================================================
 * 二、状态
 * ========================================================================== */
static uint8_t  s_initiator   = 0U;   /* 本次连接是不是本机发起的 */
static uint8_t  s_reconnectOn = 0U;
static uint8_t  s_tryCount    = 0U;
static uint32_t s_discMs      = 0U;
static uint32_t s_lastTryMs   = 0U;
static uint32_t s_lastSendMs  = 0U;
static uint32_t s_lastForceMs = 0U;
static uint8_t  s_readyScanOff = 0U;
static uint8_t  s_bondPending  = 0U;   /* 有新 Key 待写 Flash */
static uint32_t s_bondReqMs    = 0U;

uint8_t bt_input_is_initiator(void)
{
    return s_initiator;
}

/* 收到 Link Key 只登记，别在事件处理里擦 Flash：
 * FLASH_ErasePage 执行期间 CPU 取指会被挂住几十毫秒，串口收到的字节
 * 没有中断去搬，HCI 流一旦错位这条连接就废了。 */
void bt_input_request_bond_save(void)
{
    s_bondPending = 1U;
    s_bondReqMs   = hal_time_ms();
}

/* 电脑主动连进来：本次连接由主机发起，通道由主机创建 */
void bt_input_on_peer_connect(void)
{
    s_initiator   = 0U;
    s_reconnectOn = 0U;
    s_tryCount    = 0U;
}

void bt_input_on_connected(void)
{
    s_tryCount     = 0U;
    s_reconnectOn  = 0U;
    s_readyScanOff = 0U;
}

void bt_input_on_disconnected(void)
{
    s_initiator   = 0U;
    s_discMs      = hal_time_ms();
    s_reconnectOn = (link_key_valid != 0U) ? 1U : 0U;
    s_lastTryMs   = s_discMs;
}

/* ==========================================================================
 * 三、上电流程
 * ========================================================================== */
void bt_input_init(void)
{
    bt_transport_init();
    l2cap_reset();
    bond_load();

    bt_state = 1U;

    /* 从 ESP32 bt_Init() 移植过来的 19 条命令，约 1.2 秒。
     * 每条命令之间的间隔由 bt_delay_poll() 边等边收，所以这里结束时
     * 控制器的 Command_Complete 已经全部解析过了。 */
    bt_Init();

    /* 19 条命令一个事件都没回 = 控制器根本没跑起来（补丁没灌进去 /
     * 串口不通 / 波特率不对）。只有串口日志能看出卡在哪一步，
     * 所以这里把 rda_stage 一起打出来。 */
    if (hci_rx_events == 0U)
    {
        bt_err = BT_STATE_NO_CTRL;
        UART1_Printf("[bt] FAIL: no event in %lu cmds (stage=%u)\r\n",
                     (unsigned long)hci_cmd_count, (unsigned)rda_stage);
        return;
    }

    bt_state = 2U;                  /* 可被发现，等电脑连 */

    if (link_key_valid != 0U)
    {
        s_reconnectOn = 1U;
        s_discMs      = hal_time_ms();
        s_lastTryMs   = s_discMs;
    }

    UART1_Printf("[bt] ready, name=%s mtu=%u pkts=%u evt=%lu\r\n",
                 "STM32 BT Gamepad",
                 (unsigned)acl_mtu,
                 (unsigned)acl_pkts,
                 (unsigned long)hci_rx_events);
}

/* ==========================================================================
 * 四、HID 报告：按 SDP 里那份描述符打包（16 字节，Report ID = 1）
 * --------------------------------------------------------------------------
 * 描述符字段顺序（与 sdp_attr_list 中 0x05,0x01,0x09,0x05 那条记录一致）：
 *   byte 0      Report ID = 0x01
 *   byte 1-2    X   (0..65535)
 *   byte 3-4    Y
 *   byte 5-6    Rx
 *   byte 7-8    Ry
 *   byte 9-10   Z   (10 bit + 6 bit 填充)
 *   byte 11-12  Rz  (10 bit + 6 bit 填充)
 *   byte 13     Hat (4 bit + 4 bit 填充)，0 = 中立
 *   byte 14-15  Buttons (10 bit + 6 bit 填充)
 * ========================================================================== */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

/* ADC -> 0..65535：Joystick_MapRawValue 给的是 -32767..32767 */
static uint16_t axis_u16(uint8_t axis)
{
    int16_t v = Joystick_MapRawValue(axis, Joystick_ReadAxis(axis));
    int32_t u = (int32_t)v + 32768;
    if (u < 0)
    {
        u = 0;
    }
    if (u > 65535)
    {
        u = 65535;
    }
    return (uint16_t)u;
}

static uint16_t buttons_bits(void)
{
    uint16_t b = 0U;

    /* Key_GetState / Joystick_GetKeyState 都是 0=按下、1=松开 */
    if (Key_GetState(KEY_A)  == 0U) { b |= (1U << 0); }
    if (Key_GetState(KEY_B)  == 0U) { b |= (1U << 1); }
    if (Key_GetState(KEY_X)  == 0U) { b |= (1U << 2); }
    if (Key_GetState(KEY_Y)  == 0U) { b |= (1U << 3); }
    if (Key_GetState(KEY_UP) == 0U) { b |= (1U << 4); }
    if (Key_GetState(KEY_DN) == 0U) { b |= (1U << 5); }
    if (Key_GetState(KEY_LF) == 0U) { b |= (1U << 6); }
    if (Key_GetState(KEY_RG) == 0U) { b |= (1U << 7); }
    if (Joystick_GetKeyState(JOYSTICK_L3) == 0U) { b |= (1U << 8); }
    if (Joystick_GetKeyState(JOYSTICK_R3) == 0U) { b |= (1U << 9); }

    return b;
}

static void build_report(uint8_t *r)
{
    uint16_t btns = buttons_bits();

    memset(r, 0, 16U);
    r[0] = 0x01U;                                  /* Report ID */

    put16(&r[1],  axis_u16(JOYSTICK_LX));          /* X  */
    put16(&r[3],  axis_u16(JOYSTICK_LY));          /* Y  */
    put16(&r[5],  axis_u16(JOYSTICK_RX));          /* Rx */
    put16(&r[7],  axis_u16(JOYSTICK_RY));          /* Ry */

    put16(&r[9],  0U);                             /* Z  */
    put16(&r[11], 0U);                             /* Rz */
    r[13] = 0x00U;                                 /* Hat：0 = 中立 */
    put16(&r[14], btns);
}

/* ==========================================================================
 * 五、主循环任务
 * ========================================================================== */
void bt_input_task(void)
{
    uint32_t now;

    /* 1. 收包永远排第一：拖一拍可能让 PC 的 SDP 查询超时 */
    bt_transport_poll();

    now = hal_time_ms();

    /* 2. HID 通道一就绪就关扫描（谁建的通道不重要，两条路径都在这里收口） */
    if ((bt_state < 4U) && (l2cap_hid_ready() != 0U))
    {
        bt_state = 4U;
        if (s_readyScanOff == 0U)
        {
            s_readyScanOff = 1U;
            hci_write_scan_enable(0x00U);
            UART1_SendString("[bt] HID ready, scan off\r\n");
        }
    }

    if (bt_state >= 4U)
    {
        static uint8_t lastRpt[16];
        static uint8_t haveLast = 0U;
        uint8_t rpt[17];

        /* 3. 20ms 采样一次，变化才发；没变化也每 500ms 补一包，
         *    有些主机要靠周期性报告维持输入状态 */
        if ((uint32_t)(now - s_lastSendMs) >= 20U)
        {
            uint8_t changed;

            s_lastSendMs = now;

            build_report(&rpt[1]);
            rpt[0] = 0xA1U;                        /* HIDP：INPUT report */

            changed = ((haveLast == 0U) ||
                       (memcmp(&rpt[1], lastRpt, 16U) != 0)) ? 1U : 0U;

            if ((changed != 0U) || ((uint32_t)(now - s_lastForceMs) >= 500U))
            {
                l2cap_send_hid_input(rpt, 17U);
                memcpy(lastRpt, &rpt[1], 16U);
                haveLast     = 1U;
                s_lastForceMs = now;
            }
        }
    }

    /* 3.5 链路空闲时把 Key 落到 Flash（HID 通道就绪 2 秒后，此时 PC 不再连发） */
    if ((s_bondPending != 0U) && (bt_state >= 4U) &&
        ((uint32_t)(now - s_bondReqMs) >= 2000U))
    {
        s_bondPending = 0U;

        if (l2cap_hid_ready() != 0U)
        {
            bt_input_save_bond((const uint8_t *)peer_bdaddr,
                           (const uint8_t *)link_key);
        }
    }

    /* 4. 回连：断线且 Flash 里有 Key 时，每 8 秒主动寻呼一次，最多 6 次。
     *    6 次还连不上就放弃，保持可被发现状态，让人从电脑上手动配一次。 */
    if ((s_reconnectOn != 0U) && (conn_handle == BT_HANDLE_NONE) &&
        (link_key_valid != 0U) && (s_tryCount < 6U))
    {
        if ((uint32_t)(now - s_lastTryMs) >= 8000U)
        {
            s_lastTryMs = now;
            s_tryCount++;
            s_initiator = 1U;
            UART1_Printf("[bt] reconnect try %u\r\n", (unsigned)s_tryCount);
            hci_create_connection((const uint8_t *)peer_bdaddr);
        }
    }
}

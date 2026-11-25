/* ============================================================================
 * hci_event.c —— HCI 事件分发（移植自 ESP32_VHCI_demo/main/hci_event.c）
 * ----------------------------------------------------------------------------
 * 与 ESP32 版的差别：
 *   1. 不再用 union 叠结构体（ESP32 的 Encryption_Change 就用错了布局，
 *      把 status 当成了 handle 低字节），这里全部按规范偏移逐字节取。
 *   2. Link Key 会保存下来（RAM + Flash），否则回连永远失败。
 *   3. 认证完成/加密完成之后，按"谁发起连接谁建通道"的规则推进。
 * ========================================================================== */

#include "app_bt.h"

#include <string.h>
#include <stm32f10x.h>

#include <delay.h>
#include <uart.h>

/* bt_input.c 提供的钩子 */
extern void    bt_input_save_bond(const uint8_t addr[6], const uint8_t key[16]);
extern void    bt_input_request_bond_save(void);
extern uint8_t bt_input_is_initiator(void);
extern void    bt_input_on_connected(void);
extern void    bt_input_on_disconnected(void);
extern void    bt_input_on_peer_connect(void);

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void bt_log_addr(const char *tag, const uint8_t addr[6])
{
    UART1_Printf("%s %02X:%02X:%02X:%02X:%02X:%02X\r\n", tag,
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

void hci_event_recv(const uint8_t *evt, uint16_t len)
{
    const uint8_t *p = &evt[2];       /* 参数起点 */
    uint8_t  code;
    uint8_t  plen;

    if (len < 2U)
    {
        return;
    }

    code = evt[0];
    plen = evt[1];

    if ((uint16_t)(plen + 2U) > len)
    {
        return;                       /* 长度对不上，丢弃 */
    }

    switch (code)
    {
        /* -------------------------------------------------- 连接建立/断开 */
        case EVT_Connection_Complete:
            if (plen < 11U)
            {
                break;
            }
            hci_last_status = p[0];
            if (p[0] == 0U)
            {
                conn_handle = (uint16_t)(le16(&p[1]) & 0x0FFFU);
                memcpy((void *)peer_bdaddr, &p[3], 6U);
                bt_input_on_connected();
                bt_state = 3U;
                UART1_Printf("[bt] connected hdl=%u\r\n", (unsigned)conn_handle);
                bt_log_addr("[bt] peer", peer_bdaddr);

                /* 回连由本机发起时，认证/加密/建通道都得自己来
                 * （讲师 62:00：谁发起的连接，谁负责加密和建通道） */
                if (bt_input_is_initiator() != 0U)
                {
                    hci_authentication_requested(conn_handle);
                }
            }
            else
            {
                UART1_Printf("[bt] connect fail status=0x%02X\r\n", (unsigned)p[0]);
                bt_input_on_disconnected();
            }
            break;

        case EVT_Connection_Request:
            if (plen < 10U)
            {
                break;
            }
            bt_log_addr("[bt] conn req from", p);
            if (conn_handle == BT_HANDLE_NONE)
            {
                /* 记住对端地址：后面 Link Key 请求要用它比对 */
                memcpy((void *)peer_bdaddr, p, 6U);
                bt_input_on_peer_connect();      /* 主机发起，通道由主机来建 */
                hci_accept_connection(p);
            }
            else
            {
                hci_reject_connection(p);
            }
            break;

        case EVT_Disconnection_Complete:
            if (plen < 4U)
            {
                break;
            }
            UART1_Printf("[bt] disconnected reason=0x%02X\r\n", (unsigned)p[3]);
            conn_handle = BT_HANDLE_NONE;
            l2cap_reset();
            bt_state = 2U;
            bt_input_on_disconnected();
            break;

        /* -------------------------------------------------- 安全：配对链路 */
        case EVT_Link_Key_Request:
            if (plen < 6U)
            {
                break;
            }
            if ((link_key_valid != 0U) &&
                (memcmp(p, (const void *)peer_bdaddr, 6U) == 0))
            {
                UART1_Printf("[bt] link key req -> reply saved key\r\n");
                hci_link_key_reply(p, (const uint8_t *)link_key);
            }
            else
            {
                UART1_Printf("[bt] link key req -> no key\r\n");
                hci_link_key_negative_reply(p);
            }
            break;

        case EVT_Link_Key_Notification:
            if (plen < 23U)
            {
                break;
            }
            memcpy((void *)peer_bdaddr, p, 6U);
            memcpy((void *)link_key, &p[6], 16U);
            link_key_valid = 1U;
            bt_input_request_bond_save();       /* 真正写 Flash 在主循环里等链路空闲 */
            UART1_Printf("[bt] link key stored\r\n");
            break;

        case EVT_IO_Capability_Request:
            if (plen < 6U)
            {
                break;
            }
            hci_io_capability_reply(p);
            break;

        case EVT_IO_Capability_Response:
            UART1_Printf("[bt] io cap rsp\r\n");
            break;

        case EVT_User_Confirmation_Request:
            if (plen < 6U)
            {
                break;
            }
            /* 手柄没有屏幕，直接确认（对应 IO_Capability=NoInputNoOutput） */
            hci_user_confirm_reply(p);
            break;

        case EVT_Authentication_Complete:
            if (plen < 3U)
            {
                break;
            }
            hci_last_status = p[0];
            UART1_Printf("[bt] auth complete status=0x%02X\r\n", (unsigned)p[0]);
            if (p[0] == 0U)
            {
                /* 认证过了才允许加密；回连时这一步由本机主动发起 */
                hci_set_encryption((uint16_t)(le16(&p[1]) & 0x0FFFU));
            }
            break;

        case EVT_Encryption_Change:
            if (plen < 4U)
            {
                break;
            }
            hci_last_status = p[0];
            UART1_Printf("[bt] encrypt change status=0x%02X en=%u\r\n",
                         (unsigned)p[0], (unsigned)p[3]);
            if (p[0] == 0U)
            {
                hci_read_encryption_key_size((uint16_t)(le16(&p[1]) & 0x0FFFU));

                /* 谁发起连接，谁负责建 HID 通道（讲师 62:00 原话） */
                if (bt_input_is_initiator() != 0U)
                {
                    l2cap_open_hid();
                }
            }
            break;

        /* -------------------------------------------------- 命令回执 */
        case EVT_Command_Complete:
            if (plen < 4U)
            {
                break;
            }
            {
                uint16_t op = le16(&p[1]);
                uint8_t  st = p[3];

                hci_last_status = st;

                if (op == HCI_OP_Read_Buffer_Size)
                {
                    /* 回参：status(1) + ACL_MTU(2) + SCO_MTU(1) + ACL_Pkts(2) + SCO_Pkts(2)
                     * 加上 2 字节长度头 + NumCmd(1) + opcode(2)，plen 应为 11 */
                    if (st == 0U && plen >= 11U)
                    {
                        acl_mtu  = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));
                        acl_pkts = (uint16_t)(p[7] | ((uint16_t)p[8] << 8));
                        UART1_Printf("[bt] acl mtu=%u pkts=%u\r\n",
                                     (unsigned)acl_mtu, (unsigned)acl_pkts);
                    }
                }
                else if (op == HCI_OP_Read_BD_ADDR)
                {
                    if (st == 0U && plen >= 10U)
                    {
                        bt_log_addr("[bt] local bd", &p[4]);
                    }
                }
            }
            break;

        case EVT_Command_Status:
            if (plen < 4U)
            {
                break;
            }
            hci_last_status = p[0];
            if (le16(&p[2]) == HCI_OP_Create_Connection)
            {
                /* 0x0F = pending：寻呼已启动，接下来等 Role Change / Connection Complete */
                UART1_Printf("[bt] create conn status=0x%02X\r\n", (unsigned)p[0]);
            }
            break;

        case EVT_Role_Change:
            UART1_Printf("[bt] role change -> %u\r\n",
                         (unsigned)((plen >= 8U) ? p[7] : 0xFFU));
            break;

        case EVT_Simple_Pairing_Complete:
            UART1_Printf("[bt] ssp complete status=0x%02X\r\n",
                         (unsigned)((plen >= 1U) ? p[0] : 0xFFU));
            break;

        default:
            break;
    }
}

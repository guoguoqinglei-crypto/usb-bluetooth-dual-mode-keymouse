#include <bt_hci.h>

#include <string.h>
#include <stm32f10x.h>

#include <uart.h>
#include <delay.h>

#define BT_TRACE_EVT   1

#define BT_TRACE_CMD_VERBOSE  0

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void copy_rev6(uint8_t *dst, const uint8_t *src)
{
    uint8_t i;
    for (i = 0u; i < 6u; i++)
    {
        dst[i] = src[i];
    }
}

/* 按人类习惯（高字节在前）打印地址 */
static void print_addr(const char *tag, const uint8_t *a)
{
    UART1_Printf("%s %02X:%02X:%02X:%02X:%02X:%02X\r\n", tag,
                 a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void evt_connection_complete(const uint8_t *p)
{
    uint8_t status = p[0];

    if (status != BT_ERR_SUCCESS)
    {
        UART1_Printf("[evt] conn complete FAIL status=0x%02X\r\n", status);

        /* 寻呼失败：先清连接标记，再让应用层统一恢复扫描和下一轮回连。 */
        g_conn_by_us = 0u;
        g_conn_handle = BT_HANDLE_NONE;
        g_bt_state = BT_STATE_READY;
        bt_app_on_connect_failed(status);
        return;
    }

    g_conn_handle = rd16(&p[1]);
    copy_rev6(g_peer_addr, &p[3]);

    g_bt_state = BT_STATE_CONNECTED;

    UART1_Printf("[evt] CONNECTED handle=0x%04X by_us=%u\r\n",
                 (unsigned)g_conn_handle, (unsigned)g_conn_by_us);
    print_addr("[evt] peer =", g_peer_addr);

    bt_app_on_connected();

    if (g_conn_by_us != 0u)
    {
        /* 回连：是我们发起的，认证也要我们主动踢一脚。
         * 不主动发起认证，主机会一直停在"未认证"状态，加密也就起不来。 */
        g_conn_by_us = 0u;
        bt_cmd_auth_requested();
    }
}

static void evt_connection_request(const uint8_t *p)
{
    copy_rev6(g_peer_addr, p);

    print_addr("[evt] conn request from", g_peer_addr);

    if (g_conn_handle != BT_HANDLE_NONE)
    {
        /* 已经有连接了，拒绝第二条 */
        bt_cmd_reject(BT_ERR_MAX_NUM_OF_CONNS);
        return;
    }

    /* 电脑主动连接时取消本机待发的回连，避免两条寻呼流程互相竞争。 */
    bt_app_on_peer_connect();

    /* Role = 0x01：保持从机。电脑是主机，我们只做 HID 设备。 */
    bt_cmd_accept(0x01u);
}

static void evt_disconnection_complete(const uint8_t *p)
{
    uint8_t  status = p[0];
    uint16_t handle = rd16(&p[1]);
    uint8_t  reason = p[3];

    UART1_Printf("[evt] DISCONNECTED handle=0x%04X status=%u reason=0x%02X\r\n",
                 (unsigned)handle, (unsigned)status, (unsigned)reason);

    g_bt_last_disc_reason = reason;     /* 诊断：记录最近一次断开原因码 */

    if ((status == BT_ERR_SUCCESS) && (handle == g_conn_handle))
    {
        g_conn_handle = BT_HANDLE_NONE;
        g_bt_state    = BT_STATE_READY;
        bt_l2cap_reset();
        bt_app_on_disconnected();
    }
}

static void evt_authentication_complete(const uint8_t *p)
{
    uint8_t status = p[0];

    UART1_Printf("[evt] auth complete status=0x%02X\r\n", (unsigned)status);

    if (status == BT_ERR_SUCCESS)
    {
        /* 认证过了就开加密。没有加密的话，主机会拒绝后续所有 L2CAP 连接
         * （SDP 查询直接被拒，表现为"配对成功但显示无可用服务"）。 */
        bt_cmd_set_encryption();
        return;
    }

    if ((status == BT_ERR_AUTHENTICATION_FAILURE) ||
        (status == BT_ERR_PIN_MISSING))
    {
        UART1_SendString("[evt] key mismatch -> drop bond, allow re-pair\r\n");
        bt_app_drop_bond();
        bt_cmd_scan_enable(0x03u);      /* 重新可被发现，让电脑重新配对 */
    }
}

static void evt_encryption_change(const uint8_t *p)
{
    uint8_t status   = p[0];
    uint8_t enabled  = p[3];

    UART1_Printf("[evt] encryption change status=0x%02X enabled=%u\r\n",
                 (unsigned)status, (unsigned)enabled);

    if ((status == BT_ERR_SUCCESS) && (enabled != 0u))
    {
        /* 已加密。此时主机开始做 SDP 查询，随后自己开 HID 通道。 */
        bt_app_on_encrypted();
    }
}

static void evt_link_key_request(const uint8_t *p)
{
    uint8_t addr[6];
    uint8_t key[16];

    copy_rev6(addr, p);

    if (bt_app_link_key_for(addr, key) != 0u)
    {
        memcpy(g_peer_addr, addr, 6u);
        bt_cmd_link_key_reply(key);
        UART1_Printf("[evt] link key request -> reply (have bond)\r\n");
    }
    else
    {
        memcpy(g_peer_addr, addr, 6u);
        bt_cmd_link_key_negative();
        UART1_Printf("[evt] link key request -> negative (no bond, will pair)\r\n");
    }
}

static void evt_link_key_notification(const uint8_t *p)
{
    uint8_t addr[6];

    copy_rev6(addr, p);
    /* p[6..21] = Link_Key(16)，p[22] = Key_Type */

    bt_app_on_link_key(addr, &p[6]);

    UART1_Printf("[evt] LINK KEY saved to RAM (type=%u)\r\n", (unsigned)p[22]);
}

static void evt_io_capability_request(const uint8_t *p)
{
    copy_rev6(g_peer_addr, p);
    bt_cmd_io_capability_reply();
    UART1_Printf("[evt] io capability request -> reply\r\n");
}

static void evt_user_confirmation_request(const uint8_t *p)
{
    copy_rev6(g_peer_addr, p);
    bt_cmd_user_confirm_reply();
    UART1_Printf("[evt] user confirmation -> reply\r\n");
}

static void evt_simple_pairing_complete(const uint8_t *p)
{
    UART1_Printf("[evt] simple pairing complete status=0x%02X\r\n", (unsigned)p[0]);
}

static void evt_role_change(const uint8_t *p)
{
    UART1_Printf("[evt] ROLE CHANGE status=0x%02X new_role=%u (%s)\r\n",
                 (unsigned)p[0], (unsigned)p[7],
                 (p[7] == 0u) ? "本机=Master" : "本机=Slave");
}

static void evt_number_of_completed_packets(const uint8_t *p)
{
    uint8_t n = p[0];
    uint8_t i;

    /* 每条记录 4 字节：Handle(2) + Complete_Count(2) */
    for (i = 0u; i < n; i++)
    {
        uint16_t cnt = rd16(&p[1u + (uint16_t)i * 4u + 2u]);
        g_acl_credits = (uint16_t)(g_acl_credits + cnt);
    }
    if (g_acl_credits > g_acl_pkts)
    {
        g_acl_credits = g_acl_pkts;
    }
}

static void evt_command_complete(const uint8_t *p)
{
    uint16_t opcode;
    uint8_t  status;

    g_cmd_credits = p[0];

    opcode = rd16(&p[1]);
    status = p[3];

#if BT_TRACE_CMD_VERBOSE
    UART1_Printf("[evt] cmd complete op=0x%04X status=0x%02X\r\n",
                 (unsigned)opcode, (unsigned)status);
#else
    (void)opcode;
    if (status != BT_ERR_SUCCESS)
    {
        UART1_Printf("[evt] cmd FAIL op=0x%04X status=0x%02X\r\n",
                     (unsigned)opcode, (unsigned)status);
    }
#endif

    switch (opcode)
    {
        case BT_OP_Read_Buffer_Size:
            if (status == BT_ERR_SUCCESS)
            {
                g_acl_mtu  = rd16(&p[4]);
                g_acl_pkts = rd16(&p[7]);
                g_acl_credits = g_acl_pkts;

                UART1_Printf("[evt] ACL mtu=%u pkts=%u\r\n",
                             (unsigned)g_acl_mtu, (unsigned)g_acl_pkts);
            }
            break;

        case BT_OP_Read_BD_ADDR:
            if (status == BT_ERR_SUCCESS)
            {
                copy_rev6(g_local_addr, &p[4]);
                print_addr("[evt] local addr", g_local_addr);
            }
            break;

        case BT_OP_Read_Local_Version:
            if (status == BT_ERR_SUCCESS)
            {
                UART1_Printf("[evt] version hci=%u.%u lmp=%u.%u mfr=0x%04X\r\n",
                             (unsigned)p[4], (unsigned)p[5],
                             (unsigned)p[6], (unsigned)p[7],
                             (unsigned)rd16(&p[8]));
            }
            break;

        case BT_OP_Write_Scan_Enable:
            /* 回连状态机等待这个事件后再发 Create Connection，避免在
             * Num_HCI_Command_Packets=0 时连续发两条命令。 */
            bt_app_on_scan_enable_complete(status);
            break;

        default:
            break;
    }
}

static void evt_command_status(const uint8_t *p)
{
    uint16_t opcode = rd16(&p[2]);

    g_cmd_credits = p[1];

    if (p[0] != BT_ERR_SUCCESS)
    {
        UART1_Printf("[evt] cmd status op=0x%04X err=0x%02X\r\n",
                     (unsigned)opcode, (unsigned)p[0]);

        if (opcode == BT_OP_Create_Connection)
        {
            /* 命令被直接拒绝时不会再有 Connection Complete，必须在这里
             * 退出 connecting 状态，否则以后再也不会发起下一轮回连。 */
            g_conn_by_us = 0u;
            bt_app_on_connect_failed(p[0]);
        }
    }
}

/* 每个事件至少要有多少字节的"参数体"（不含事件码和长度字节），
 * 不够就丢掉。控制器短包或分包错位时靠这个兜底，避免越界读。 */
static uint8_t evt_min_param(uint8_t code)
{
    switch (code)
    {
        case EVT_Connection_Complete:         return 11u;
        case EVT_Connection_Request:          return 10u;
        case EVT_Disconnection_Complete:      return 4u;
        case EVT_Authentication_Complete:     return 3u;
        case EVT_Encryption_Change:           return 4u;
        case EVT_Link_Key_Request:            return 6u;
        case EVT_Link_Key_Notification:       return 23u;
        case EVT_IO_Capability_Request:       return 6u;
        case EVT_User_Confirmation_Request:   return 10u;
        case EVT_Simple_Pairing_Complete:     return 7u;
        case EVT_Role_Change:                 return 8u;
        case EVT_Mode_Change:                 return 6u;
        case EVT_Number_Of_Completed_Packets: return 1u;
        case EVT_Command_Complete:            return 4u;
        case EVT_Command_Status:              return 4u;
        case EVT_Link_Supervision_Timeout_Changed:     return 4u;
        case EVT_Link_Supervision_Timeout_Changed_Std: return 4u;
        default:                              return 0u;
    }
}

void bt_evt_dispatch(const uint8_t *params, uint8_t len)
{
    uint8_t code;
    uint8_t need;

    if (len < 2u)
    {
        return;                 /* 至少要有 事件码 + 长度 */
    }

    code = params[0];
    g_evt_count++;

    need = evt_min_param(code);
    if ((uint8_t)(len - 2u) < need)
    {
        /* 参数不够，丢掉这个事件（不猜） */
        return;
    }

    switch (code)
    {
        case EVT_Connection_Complete:      evt_connection_complete(&params[2]);      break;
        case EVT_Connection_Request:       evt_connection_request(&params[2]);       break;
        case EVT_Disconnection_Complete:   evt_disconnection_complete(&params[2]);   break;
        case EVT_Authentication_Complete:  evt_authentication_complete(&params[2]);  break;
        case EVT_Encryption_Change:        evt_encryption_change(&params[2]);        break;
        case EVT_Link_Key_Request:         evt_link_key_request(&params[2]);         break;
        case EVT_Link_Key_Notification:    evt_link_key_notification(&params[2]);    break;
        case EVT_IO_Capability_Request:    evt_io_capability_request(&params[2]);    break;
        case EVT_User_Confirmation_Request:evt_user_confirmation_request(&params[2]);break;
        case EVT_Simple_Pairing_Complete:  evt_simple_pairing_complete(&params[2]);  break;
        case EVT_Number_Of_Completed_Packets: evt_number_of_completed_packets(&params[2]); break;
        case EVT_Command_Complete:         evt_command_complete(&params[2]);         break;
        case EVT_Command_Status:           evt_command_status(&params[2]);           break;
        case EVT_Role_Change:              evt_role_change(&params[2]);              break;

        case EVT_Link_Supervision_Timeout_Changed:
        case EVT_Link_Supervision_Timeout_Changed_Std:
            UART1_Printf("[evt] supervision timeout CHANGED h=0x%04X val=%lu ms (link still up)\r\n",
                         (unsigned)rd16(&params[2]),
                         (unsigned long)rd16(&params[4]) * 5UL / 8UL);
            break;

        default:
            /* 以前这里是空的，等于把不认识的事件全吞了。
             * 现在连原始字节一起打出来 —— 只看事件码和长度没法判断是什么。 */
            {
                uint8_t  n = (uint8_t)(len - 2u);
                uint8_t  k;
                uint8_t  show = (n > 12u) ? 12u : n;

                UART1_Printf("[evt] ? code=0x%02X len=%u :",
                             (unsigned)code, (unsigned)n);
                for (k = 0u; k < show; k++)
                {
                    UART1_Printf(" %02X", (unsigned)params[2u + k]);
                }
                UART1_SendString("\r\n");
            }
            break;
    }
}

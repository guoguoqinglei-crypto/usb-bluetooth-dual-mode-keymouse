#include <bt_hci.h>

#include <string.h>
#include <stm32f10x.h>

#include <bt_uart.h>
#include <uart.h>
#include <delay.h>

/* 蓝牙设备名（EIR 里以 UTF-8 下发；建议用英文，主机显示最稳） */
#define DEV_NAME   "MP KEYMOUSE BT"

static const uint8_t s_cod[3] = { 0xC0u, 0x05u, 0x00u };

/* 事件掩码：全开（讲师版演示用 FF…3F，这里是它的超集，多开只会多收事件，
 * 不会漏掉连接请求 / 链路密钥请求这些关键事件）。 */
static const uint8_t s_eventMask[8] =
{
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x3Fu
};

static const uint8_t s_linkPolicy[2] = { 0x00u, 0x00u };

/* 查询扫描：间隔 0x0200 = 320ms，窗口 0x0060 = 60ms */
static const uint8_t s_inqScan[4] = { 0x00u, 0x02u, 0x60u, 0x00u };

/* 寻呼扫描：间隔 0x0480 = 720ms，窗口 0x0080 = 80ms */
static const uint8_t s_pageScan[4] = { 0x80u, 0x04u, 0x80u, 0x00u };

static uint8_t s_nameBuf[248];
static uint8_t s_eirBuf[241];      /* [0]=FEC_Required，[1..240]=EIR 数据 */

static void bt_wait(uint16_t ms)
{
    uint16_t i;

    for (i = 0u; i < ms; i++)
    {
        bt_transport_poll();
        Delay_ms(1);
    }
    bt_transport_poll();
}

static void bt_send_paced(uint16_t opcode, const uint8_t *p, uint8_t plen, uint16_t gap)
{
    bt_send_cmd(opcode, p, plen);
    bt_wait(gap);
}

static void bt_build_eir(void)
{
    uint8_t  nlen = (uint8_t)strlen(DEV_NAME);
    uint16_t i;

    memset(s_eirBuf, 0, sizeof(s_eirBuf));

    s_eirBuf[0] = 0x00u;                      /* FEC 不要求（想开就改 0x01） */
    s_eirBuf[1] = (uint8_t)(nlen + 1u);       /* 本段长度 = 类型(1) + 名字 */
    s_eirBuf[2] = 0x09u;                      /* 0x09 = Complete Local Name */
    for (i = 0u; i < nlen; i++)
    {
        s_eirBuf[3u + i] = (uint8_t)DEV_NAME[i];
    }
}

uint8_t bt_init_sequence(void)
{
    uint8_t  one;
    uint8_t  iac[4];
    uint8_t  hostBuf[7];
    uint8_t  extFeatPage = 0x01u;
    uint16_t pkts;
    uint32_t evt0 = g_evt_count;

    /* 1. 复位控制器。厂商补丁灌完后芯片已就绪，这一步把内部状态清零，
     *    保证后面写进去的名称/CoD/扫描参数不会和残留状态打架。 */
    bt_send_paced(BT_OP_Reset, 0, 0u, 250u);

    /* 2~6. 读类命令：版本、特性、地址、缓冲大小 */
    bt_send_paced(BT_OP_Read_Local_Features, 0, 0u, 40u);
    bt_send_paced(BT_OP_Read_Local_Version, 0, 0u, 40u);
    bt_send_paced(BT_OP_Read_BD_ADDR, 0, 0u, 40u);
    bt_send_paced(BT_OP_Read_Local_Ext_Features, &extFeatPage, 1u, 40u);
    bt_send_paced(BT_OP_Read_Buffer_Size, 0, 0u, 40u);

    /* 7. 打开安全简单配对（SSP）。不打开就会退回 Legacy PIN 配对，
     *    设备没有键盘，电脑会弹 PIN 框而无法完成。 */
    one = 0x01u;
    bt_send_paced(BT_OP_Write_Simple_Pairing_Mode, &one, 1u, 40u);

    /* 8. 查询响应模式：0x02 = 带 RSSI 的扩展查询响应 */
    one = 0x02u;
    bt_send_paced(BT_OP_Write_Inquiry_Mode, &one, 1u, 40u);

    /* 9. 默认链路策略 */
    bt_send_paced(BT_OP_Write_Default_Link_Policy, s_linkPolicy, 2u, 40u);

    /* 10. 设备类别 = 键鼠组合 */
    bt_send_paced(BT_OP_Write_Class_Of_Device, s_cod, 3u, 40u);

    /* 11. 本地名称（固定 248 字节，不足补 0） */
    memset(s_nameBuf, 0, sizeof(s_nameBuf));
    memcpy(s_nameBuf, DEV_NAME, strlen(DEV_NAME));
    bt_send_paced(BT_OP_Write_Local_Name, s_nameBuf, 248u, 60u);

    /* 12. EIR 扩展查询响应（241 字节） */
    bt_build_eir();
    bt_send_paced(BT_OP_Write_EIR, s_eirBuf, (uint8_t)sizeof(s_eirBuf), 40u);

    /* 13. 事件掩码。这一条不发，控制器收到电脑的连接请求也不会告诉我们，
     *      表现就是"能搜到、点连接没反应"。 */
    bt_send_paced(BT_OP_Set_Event_Mask, s_eventMask, 8u, 40u);

    /* 14~17. 扫描的间隔/窗口/类型 */
    bt_send_paced(BT_OP_Write_Inquiry_Scan_Activity, s_inqScan, 4u, 40u);
    one = 0x01u;
    bt_send_paced(BT_OP_Write_Inquiry_Scan_Type, &one, 1u, 40u);
    bt_send_paced(BT_OP_Write_Page_Scan_Activity, s_pageScan, 4u, 40u);
    one = 0x01u;
    bt_send_paced(BT_OP_Write_Page_Scan_Type, &one, 1u, 40u);

    /* 通用查询访问码 GIAC = 0x9E8B33，必须和电脑一致，否则搜不到 */
    iac[0] = 0x01u;                   /* Num_Current_IAC */
    iac[1] = 0x33u;
    iac[2] = 0x8Bu;
    iac[3] = 0x9Eu;
    bt_send_paced(BT_OP_Write_Current_IAC_LAP, iac, 4u, 40u);

    /* 主机侧缓冲声明：告诉控制器"我这边一次能接多少" */
    pkts = (g_acl_pkts >= 2u) ? g_acl_pkts : 2u;
    if (pkts > 8u)
    {
        pkts = 8u;
    }
    hostBuf[0] = (uint8_t)(g_acl_mtu & 0xFFu);
    hostBuf[1] = (uint8_t)(g_acl_mtu >> 8);
    hostBuf[2] = 0u;                            /* SCO 长度，不用 */
    hostBuf[3] = (uint8_t)(pkts & 0xFFu);
    hostBuf[4] = (uint8_t)(pkts >> 8);
    hostBuf[5] = 0u;
    hostBuf[6] = 0u;
    bt_send_paced(BT_OP_Host_Buffer_Size, hostBuf, 7u, 40u);

    /* 【实验 5：去掉 Set_Host_Flow_Control】
     *
     * 这条命令（Host_Flow_Control=1）在蓝牙 4.0 已废弃，作用是让控制器
     * 把 ACL 缓冲完全交给主机管理：控制器发数据前必须等主机归还"信用"，
     * 归还不及时控制器就会自己卡住，最后很可能把链路拆掉
     * （断开原因 0x13 Remote User Terminated）。
     *
     * 已验证稳定的手柄工程里没有这条命令，所以这里也去掉，
     * 让控制器自己管缓冲（Host_Buffer_Size 仍然声明，符合常规做法）。 */
    /* one = 0x01u; */
    /* bt_send_paced(BT_OP_Set_Host_Flow_Control, &one, 1u, 40u); */

    /* 18. 最后打开查询扫描 + 寻呼扫描：可被发现 + 可被连接 */
    one = 0x03u;
    bt_send_paced(BT_OP_Write_Scan_Enable, &one, 1u, 60u);

    /* 初始化期间只要收到过任何一个事件，就说明链路是通的 */
    return (g_evt_count != evt0) ? 1u : 0u;
}

void bt_cmd_accept(uint8_t role)
{
    uint8_t p[7];

    memcpy(p, g_peer_addr, 6u);
    p[6] = role;                 /* 0x01 = 保持从机（电脑当主机） */
    bt_send_cmd(BT_OP_Accept_Connection_Request, p, 7u);
}

void bt_cmd_reject(uint8_t reason)
{
    uint8_t p[7];

    memcpy(p, g_peer_addr, 6u);
    p[6] = reason;
    bt_send_cmd(BT_OP_Reject_Connection_Request, p, 7u);
}

void bt_cmd_disconnect(uint16_t handle, uint8_t reason)
{
    uint8_t p[3];

    p[0] = (uint8_t)(handle & 0xFFu);      /* Connection_Handle 低字节 */
    p[1] = (uint8_t)(handle >> 8);         /* 高字节 */
    p[2] = reason;                         /* 0x13 = Remote User Terminated */

    bt_send_cmd(BT_OP_Disconnect, p, 3u);
}

void bt_cmd_link_key_reply(const uint8_t key[16])
{
    uint8_t p[22];

    memcpy(p, g_peer_addr, 6u);
    memcpy(&p[6], key, 16u);
    bt_send_cmd(BT_OP_Link_Key_Request_Reply, p, 22u);
}

void bt_cmd_link_key_negative(void)
{
    bt_send_cmd(BT_OP_Link_Key_Request_Neg_Reply, g_peer_addr, 6u);
}

void bt_cmd_io_capability_reply(void)
{
    uint8_t p[9];

    memcpy(p, g_peer_addr, 6u);
    /* NoInputNoOutput：设备没有键盘也没有屏幕，走 Just Works，
     * 电脑端不弹 PIN 码，点一下就配对完成 */
    p[6] = 0x03u;
    p[7] = 0x00u;                /* 无 OOB 数据 */
    /* Authentication_Requirements：0x02 = 不要求 MITM + 通用绑定。
     * 用 NoInputNoOutput 时不能要求 MITM，否则部分主机会直接拒绝配对。 */
    p[8] = 0x02u;
    bt_send_cmd(BT_OP_IO_Capability_Request_Reply, p, 9u);
}

void bt_cmd_user_confirm_reply(void)
{
    /* User Confirmation Request 的参数只有 BD_ADDR + 数值，
     * 回复也只带 BD_ADDR（6 字节），多带字节会被当成参数长度错误 */
    bt_send_cmd(BT_OP_User_Confirmation_Reply, g_peer_addr, 6u);
}

void bt_cmd_auth_requested(void)
{
    uint8_t p[2];

    p[0] = (uint8_t)(g_conn_handle & 0xFFu);
    p[1] = (uint8_t)(g_conn_handle >> 8);
    bt_send_cmd(BT_OP_Authentication_Requested, p, 2u);
}

void bt_cmd_set_encryption(void)
{
    uint8_t p[3];

    p[0] = (uint8_t)(g_conn_handle & 0xFFu);
    p[1] = (uint8_t)(g_conn_handle >> 8);
    p[2] = 0x01u;                /* 1 = 打开加密 */
    bt_send_cmd(BT_OP_Set_Connection_Encryption, p, 3u);
}

void bt_cmd_scan_enable(uint8_t en)
{
    bt_send_cmd(BT_OP_Write_Scan_Enable, &en, 1u);
}

void bt_cmd_create_connection(const uint8_t addr[6])
{
    uint8_t p[13];

    memcpy(p, addr, 6u);
    p[6]  = 0x18u;
    p[7]  = 0xCCu;
    p[8]  = 0x00u;               /* Page_Scan_Repetition_Mode = R0 */
    p[9]  = 0x00u;               /* Reserved */
    p[10] = 0x00u;               /* Clock_Offset 低 */
    p[11] = 0x00u;               /* Clock_Offset 高 */
    p[12] = 0x01u;               /* 允许角色切换 */

    bt_send_cmd(BT_OP_Create_Connection, p, 13u);
}

void bt_cmd_set_supervision_timeout(void)
{
    uint8_t p[4];

    if (g_conn_handle == BT_HANDLE_NONE)
    {
        return;
    }

    p[0] = (uint8_t)(g_conn_handle & 0xFFu);
    p[1] = (uint8_t)(g_conn_handle >> 8);
    p[2] = 0x00u;                /* 0x7D00 小端 */
    p[3] = 0x7Du;

    bt_send_cmd(BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Link_Supervision_Timeout), p, 4u);
}

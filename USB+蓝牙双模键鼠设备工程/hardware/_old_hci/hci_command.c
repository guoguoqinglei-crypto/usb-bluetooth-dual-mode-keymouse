/* ============================================================================
 * hci_command.c —— HCI 命令层
 * ----------------------------------------------------------------------------
 * 对应 ESP32_VHCI_demo/main/hci_command.c，函数名保持一致：
 *   uart_send()    发一包完整的 H4 数据
 *   hci_send_cmd() 组装并发送一条命令
 *   bt_Init()      demo 的初始化命令序列（这里 19 条，含读缓冲大小）
 *
 * 与 demo 唯一的区别：demo 的 uart_send() 里带 vTaskDelay(40)，
 * 这里改成"发完就返回"，40ms 的节奏交给 bt_cmd_paced()，
 * 因为 STM32 在等待期间必须继续收事件，不能用死延时。
 * ========================================================================== */

#include "app_bt.h"

#include <string.h>
#include <stm32f10x.h>

#include <delay.h>
#include <bt_uart.h>
#include <uart.h>

#define BT_TRACE  1

/* 初始化序列开头要不要补一条 HCI_Reset。
 * RDA5876 的厂商补丁表最后已经做过一次核心复位，此时波特率/PS 配置刚生效，
 * 再 Reset 有把链路打回 115200 的风险（那就等于 200 多条补丁白写了），
 * 所以默认不发。哪天上电后扫描没打开，可以先把它改成 1 试试。 */
#define BT_USE_HCI_RESET  0

/* 一包 H4 数据发出去（命令/ACL 通用） */
void uart_send(const uint8_t *buf, uint16_t len)
{
    BtUart_Send(buf, len);
}

/* ==========================================================================
 * 二、发送
 * ========================================================================== */

/* 命令缓冲放静态区：主栈只有 1KB，259 字节的局部数组容易踩栈 */
static uint8_t s_cmdBuf[4U + 255U];

uint8_t hci_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    s_cmdBuf[0] = H4_TYPE_CMD;
    s_cmdBuf[1] = (uint8_t)(opcode & 0xFFU);
    s_cmdBuf[2] = (uint8_t)(opcode >> 8);
    s_cmdBuf[3] = plen;

    if ((plen > 0U) && (params != 0))
    {
        memcpy(&s_cmdBuf[4], params, plen);
    }

    hci_cmd_count++;

#if BT_TRACE
    UART1_Printf("TX cmd %04X len=%u\r\n", (unsigned)opcode, (unsigned)plen);
#endif

    uart_send(s_cmdBuf, (uint16_t)(4U + plen));
    return 1U;
}

/* 发一个 ACL 数据包：H4 0x02 | handle(2,含 PB) | len(2) | data */
/* ==========================================================================
 * 三、初始化命令序列（对应 ESP32 的 bt_Init()）
 * ========================================================================== */
static const char     s_devName[]  = "STM32 BT Gamepad";
static const uint8_t  s_cod[3]     = {0x08U, 0x25U, 0x00U};   /* 与 ESP32 demo 一致 */
static const uint8_t  s_linkPolicy[2] = {0x07U, 0x00U};       /* role switch + hold + sniff */
static const uint8_t  s_inqScan[4] = {0x00U, 0x02U, 0x60U, 0x00U}; /* 320ms / 60ms */
static const uint8_t  s_pageScan[4] = {0x80U, 0x04U, 0x80U, 0x00U}; /* 720ms / 80ms */
static const uint8_t  s_eventMask[8] =
{
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x3FU
};

static uint8_t s_nameParam[248];

/* Write_Extended_Inquiry_Response 的参数是 241 字节：
 *   [0]     FEC_Required（1 字节）
 *   [1..240] Extended_Inquiry_Response（240 字节）
 * Core v5.4 Vol4 PartE §7.3.56 明确写了这两个字段。
 * 早期版本只发 240 字节且把 EIR 长度字节放在 [0]，
 * 结果 FEC 字段被吃掉、整个 EIR 错位，设备名广播不出去（电脑显示"未知设备"）。 */
static uint8_t s_eirParam[241];

/* 边等边收：把一段延时切成 1ms 一格，每格都把串口收到的字节拆出来分发。
 *
 * 这一条是必须的，不能只用 Delay_ms：
 *   Read_Buffer_Size / Read_BD_ADDR 的返回值是在 Command_Complete 事件里
 *   带回来的，如果初始化期间光发不收，这两个值永远拿不到，
 *   acl_mtu 会一直停在 128，后面 SDP 的 662 字节响应就分片错乱。 */
static void bt_delay_poll(uint16_t ms)
{
    uint16_t i;

    for (i = 0U; i < ms; i++)
    {
        bt_transport_poll();
        Delay_ms(1);
    }
    bt_transport_poll();
}

/* 一条命令 + 一段间隔：与 ESP32 的 uart_send() 节奏一致，
 * 慢是为了让芯片把 Command Complete 吐完，初始化只在开机跑一次 */
static void bt_cmd_paced(uint16_t opcode, const uint8_t *params, uint8_t plen,
                         uint16_t gap_ms)
{
    hci_send_cmd(opcode, params, plen);
    bt_delay_poll(gap_ms);
}

/* 按 EIR 规范组广播数据：FEC(1) + [len | 0x09 | 名字…] */
static void bt_build_eir(void)
{
    uint8_t nlen = (uint8_t)strlen(s_devName);
    uint16_t i;

    memset(s_eirParam, 0, sizeof(s_eirParam));

    /* [0] FEC_Required：0x01 = 要求 FEC（与 ESP32 demo 一致），
     *     0x00 = 不要求。若上电后设备名广播不出来，先试改成 0x00。 */
    s_eirParam[0] = 0x01U;

    s_eirParam[1] = (uint8_t)(nlen + 1U);   /* 本段长度 = 类型(1) + 名字 */
    s_eirParam[2] = 0x09U;                  /* EIR 类型：Complete Local Name */
    for (i = 0U; i < nlen; i++)
    {
        s_eirParam[3U + i] = (uint8_t)s_devName[i];
    }
}

void bt_Init(void)
{
    uint8_t  one;
    uint8_t  iac[4];
    uint8_t  hostBuf[7];
    uint16_t pkts;

#if BT_USE_HCI_RESET
    /* 1. 复位控制器（等 250ms 让固件重新就绪，名称/CoD/扫描都会被清） */
    bt_cmd_paced(HCI_OP_Reset, 0, 0U, 250U);
#endif

    /* 2~5. 读版本 / 特性 / 地址 / 缓冲大小 */
    bt_cmd_paced(HCI_OPCODE(OGF_INFO_PARAMS, 0x001), 0, 0U, 40U);   /* Read Local Version */
    bt_cmd_paced(HCI_OPCODE(OGF_INFO_PARAMS, 0x003), 0, 0U, 40U);   /* Read Local Features */
    bt_cmd_paced(HCI_OP_Read_BD_ADDR, 0, 0U, 40U);
    bt_cmd_paced(HCI_OP_Read_Buffer_Size, 0, 0U, 40U);

    /* 6. 查询响应模式：0x02 = 带 RSSI 的 EIR */
    one = 0x02U;
    bt_cmd_paced(HCI_OP_Write_Inquiry_Mode, &one, 1U, 40U);

    /* 7. 打开 SSP 简单配对 */
    one = 0x01U;
    bt_cmd_paced(HCI_OP_Write_Simple_Pairing_Mode, &one, 1U, 40U);

    /* 8. 默认链路策略：允许角色切换 / hold / sniff */
    bt_cmd_paced(HCI_OP_Write_Default_Link_Policy, s_linkPolicy, 2U, 40U);

    /* 9. 设备类别（手柄） */
    bt_cmd_paced(HCI_OP_Write_Class_Of_Device, s_cod, 3U, 40U);

    /* 10~13. 查询扫描 / 寻呼扫描的间隔、窗口、类型 */
    bt_cmd_paced(HCI_OP_Write_Inquiry_Scan_Activity, s_inqScan, 4U, 40U);
    one = 0x01U;
    bt_cmd_paced(HCI_OP_Write_Inquiry_Scan_Type, &one, 1U, 40U);
    bt_cmd_paced(HCI_OP_Write_Page_Scan_Activity, s_pageScan, 4U, 40U);
    one = 0x01U;
    bt_cmd_paced(HCI_OP_Write_Page_Scan_Type, &one, 1U, 40U);

    /* 14. 通用查询访问码 GIAC = 0x9E8B33（必须和电脑一致，否则搜不到） */
    iac[0] = 0x01U;
    iac[1] = 0x33U;
    iac[2] = 0x8BU;
    iac[3] = 0x9EU;
    bt_cmd_paced(HCI_OP_Write_Current_IAC_LAP, iac, 4U, 40U);

    /* 15. EIR：把设备名放进广播（241 字节 = FEC + 240 字节 EIR） */
    bt_build_eir();
    bt_cmd_paced(HCI_OP_Write_Extended_Inquiry_Rsp, s_eirParam,
                 (uint8_t)sizeof(s_eirParam), 40U);

    /* 16. 事件掩码：不设这一条，控制器触发事件时不会回传（最常见的坑） */
    bt_cmd_paced(HCI_OP_Set_Event_Mask, s_eventMask, 8U, 40U);

    /* 17. 本地名称（固定 248 字节，不足补 0） */
    memset(s_nameParam, 0, sizeof(s_nameParam));
    memcpy(s_nameParam, s_devName, strlen(s_devName));
    bt_cmd_paced(HCI_OP_Write_Local_Name, s_nameParam, 248U, 60U);

    /* 18. 主机侧 ACL 缓冲声明：告诉控制器我们一次最多要几个包 */
    pkts = (acl_pkts >= 2U) ? acl_pkts : 2U;
    if (pkts > 8U)
    {
        pkts = 8U;
    }
    hostBuf[0] = (uint8_t)(acl_mtu & 0xFFU);
    hostBuf[1] = (uint8_t)(acl_mtu >> 8);
    hostBuf[2] = 0U;                       /* SCO MTU，不用 */
    hostBuf[3] = (uint8_t)(pkts & 0xFFU);
    hostBuf[4] = (uint8_t)(pkts >> 8);
    hostBuf[5] = 0U;
    hostBuf[6] = 0U;
    bt_cmd_paced(HCI_OP_Host_Buffer_Size, hostBuf, 7U, 40U);

    /* 19. 打开查询扫描 + 寻呼扫描：电脑才能搜到、才能连进来 */
    one = 0x03U;
    bt_cmd_paced(HCI_OP_Write_Scan_Enable, &one, 1U, 60U);
}

/* ==========================================================================
 * 五、事件应答用的单发命令
 * ========================================================================== */
uint8_t hci_write_scan_enable(uint8_t enable)
{
    return hci_send_cmd(HCI_OP_Write_Scan_Enable, &enable, 1U);
}

uint8_t hci_accept_connection(const uint8_t addr[6])
{
    uint8_t p[7];
    memcpy(p, addr, 6U);
    p[6] = 0x01U;              /* Link_Role：0x01 = 保持从机（电脑是主机） */
    return hci_send_cmd(HCI_OP_Accept_Connection_Request, p, 7U);
}

uint8_t hci_reject_connection(const uint8_t addr[6])
{
    uint8_t p[7];
    memcpy(p, addr, 6U);
    p[6] = BT_ERR_MAX_NUM_OF_CONNS;
    return hci_send_cmd(HCI_OP_Reject_Connection_Request, p, 7U);
}

uint8_t hci_link_key_reply(const uint8_t addr[6], const uint8_t key[16])
{
    uint8_t p[22];
    memcpy(p, addr, 6U);
    memcpy(&p[6], key, 16U);
    return hci_send_cmd(HCI_OP_Link_Key_Request_Reply, p, 22U);
}

uint8_t hci_link_key_negative_reply(const uint8_t addr[6])
{
    return hci_send_cmd(HCI_OP_Link_Key_Request_Neg_Reply, addr, 6U);
}

uint8_t hci_io_capability_reply(const uint8_t addr[6])
{
    uint8_t p[9];
    memcpy(p, addr, 6U);
    p[6] = 0x03U;              /* NoInputNoOutput：手柄没有键盘，避免弹 PIN 框 */
    p[7] = 0x00U;              /* 无 OOB 数据 */
    p[8] = 0x04U;              /* Authentication_Requirements：专用绑定 */
    return hci_send_cmd(HCI_OP_IO_Capability_Request_Reply, p, 9U);
}

uint8_t hci_user_confirm_reply(const uint8_t addr[6])
{
    return hci_send_cmd(HCI_OP_User_Confirmation_Reply, addr, 6U);
}

uint8_t hci_authentication_requested(uint16_t handle)
{
    uint8_t p[2];
    p[0] = (uint8_t)(handle & 0xFFU);
    p[1] = (uint8_t)(handle >> 8);
    return hci_send_cmd(HCI_OP_Authentication_Requested, p, 2U);
}

uint8_t hci_set_encryption(uint16_t handle)
{
    uint8_t p[3];
    p[0] = (uint8_t)(handle & 0xFFU);
    p[1] = (uint8_t)(handle >> 8);
    p[2] = 0x01U;              /* Encryption_Enable：1 = 打开加密 */
    return hci_send_cmd(HCI_OP_Set_Connection_Encryption, p, 3U);
}

uint8_t hci_read_encryption_key_size(uint16_t handle)
{
    uint8_t p[2];
    p[0] = (uint8_t)(handle & 0xFFU);
    p[1] = (uint8_t)(handle >> 8);
    return hci_send_cmd(HCI_OP_Read_Encryption_Key_Size, p, 2U);
}

uint8_t hci_create_connection(const uint8_t addr[6])
{
    uint8_t p[13];

    memcpy(p, addr, 6U);
    /* Packet_Type：DM1/DH1/DM3/DH3/DM5/DH5 全开 = 0xCC18（小端 18 CC） */
    p[6]  = 0x18U;
    p[7]  = 0xCCU;
    p[8]  = 0x00U;             /* Page_Scan_Repetition_Mode = R0 */
    p[9]  = 0x00U;             /* Reserved */
    p[10] = 0x00U;             /* Clock_Offset 低 */
    p[11] = 0x00U;             /* Clock_Offset 高 */
    p[12] = 0x01U;             /* 允许角色切换 */

    return hci_send_cmd(HCI_OP_Create_Connection, p, 13U);
}

#ifndef __BT_HCI_H
#define __BT_HCI_H

#include <stdint.h>

#define H4_TYPE_CMD      0x01u
#define H4_TYPE_ACL      0x02u
#define H4_TYPE_SCO      0x03u
#define H4_TYPE_EVENT    0x04u

#define OGF_LINK_CTRL       0x01u
#define OGF_LINK_POLICY     0x02u
#define OGF_CTRL_BASEBAND   0x03u
#define OGF_INFO_PARAM      0x04u
#define OGF_STATUS_PARAM    0x05u
#define OGF_TESTING         0x06u

#define BT_OPCODE(ogf, ocf) \
    ((uint16_t)((((uint16_t)(ogf)) << 10) | ((uint16_t)(ocf) & 0x03FFu)))

/* ---- OGF 0x01 链路控制 ---- */
#define OCF_Inquiry                     0x001u
#define OCF_Inquiry_Cancel              0x002u
#define OCF_Create_Connection           0x005u
#define OCF_Disconnect                  0x006u
#define OCF_Accept_Connection_Request   0x009u
#define OCF_Reject_Connection_Request   0x00Au
#define OCF_Link_Key_Request_Reply      0x00Bu
#define OCF_Link_Key_Request_Neg_Reply  0x00Cu
#define OCF_PIN_Code_Request_Reply      0x00Du
#define OCF_PIN_Code_Request_Neg_Reply  0x00Eu
#define OCF_Authentication_Requested    0x011u
#define OCF_Set_Connection_Encryption   0x013u
#define OCF_IO_Capability_Request_Reply 0x02Bu
#define OCF_User_Confirmation_Reply     0x02Cu
#define OCF_User_Passkey_Request_Reply  0x02Eu

/* ---- OGF 0x02 链路策略 ---- */
#define OCF_Hold_Mode                   0x001u
#define OCF_Sniff_Mode                  0x003u
#define OCF_Exit_Sniff_Mode             0x004u
#define OCF_Switch_Role                 0x00Bu
#define OCF_Write_Link_Policy_Settings  0x00Du

/* ---- OGF 0x03 控制器与基带 ---- */
#define OCF_Set_Event_Mask              0x001u
#define OCF_Reset                       0x003u
#define OCF_Set_Event_Filter            0x005u
#define OCF_Flush                       0x008u
#define OCF_Write_Local_Name            0x013u
#define OCF_Read_Scan_Enable            0x019u
#define OCF_Write_Scan_Enable           0x01Au
#define OCF_Write_Page_Scan_Activity    0x01Cu
#define OCF_Write_Inquiry_Scan_Activity 0x01Eu
#define OCF_Write_Class_Of_Device       0x024u
#define OCF_Write_Voice_Setting         0x026u
#define OCF_Set_Controller_To_Host_Flow_Control 0x031u
#define OCF_Host_Buffer_Size            0x033u
#define OCF_Host_Number_Of_Completed_Packets 0x035u
#define OCF_Write_Current_IAC_LAP       0x03Au
#define OCF_Write_Inquiry_Scan_Type     0x043u
#define OCF_Write_Inquiry_Mode          0x045u
#define OCF_Write_Page_Scan_Type        0x047u
#define OCF_Write_Extended_Inquiry_Rsp  0x052u
#define OCF_Write_Simple_Pairing_Mode   0x056u
#define OCF_Write_Default_Link_Policy_Settings 0x00Fu
#define OCF_Write_Page_Timeout          0x018u
#define OCF_Write_Connection_Accept_Timeout 0x016u
#define OCF_Write_Link_Supervision_Timeout 0x037u

/* ---- OGF 0x04 信息参数 ---- */
#define OCF_Read_Local_Version          0x001u
#define OCF_Read_Local_Supported_Cmds   0x002u
#define OCF_Read_Local_Supported_Feat   0x003u
#define OCF_Read_Local_Extended_Feat    0x004u
#define OCF_Read_Buffer_Size            0x005u
#define OCF_Read_BD_ADDR                0x009u

/* ---- OGF 0x05 状态参数 ---- */
#define OCF_Read_Encryption_Key_Size    0x008u

/* 组好的常用 opcode */
#define BT_OP_Accept_Connection_Request   BT_OPCODE(OGF_LINK_CTRL, OCF_Accept_Connection_Request)
#define BT_OP_Reject_Connection_Request   BT_OPCODE(OGF_LINK_CTRL, OCF_Reject_Connection_Request)
#define BT_OP_Create_Connection           BT_OPCODE(OGF_LINK_CTRL, OCF_Create_Connection)
#define BT_OP_Link_Key_Request_Reply      BT_OPCODE(OGF_LINK_CTRL, OCF_Link_Key_Request_Reply)
#define BT_OP_Link_Key_Request_Neg_Reply  BT_OPCODE(OGF_LINK_CTRL, OCF_Link_Key_Request_Neg_Reply)
#define BT_OP_Authentication_Requested    BT_OPCODE(OGF_LINK_CTRL, OCF_Authentication_Requested)
#define BT_OP_Set_Connection_Encryption   BT_OPCODE(OGF_LINK_CTRL, OCF_Set_Connection_Encryption)
#define BT_OP_IO_Capability_Request_Reply BT_OPCODE(OGF_LINK_CTRL, OCF_IO_Capability_Request_Reply)
#define BT_OP_User_Confirmation_Reply     BT_OPCODE(OGF_LINK_CTRL, OCF_User_Confirmation_Reply)
#define BT_OP_Disconnect                  BT_OPCODE(OGF_LINK_CTRL, OCF_Disconnect)

#define BT_OP_Set_Event_Mask              BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Set_Event_Mask)
#define BT_OP_Reset                       BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Reset)
#define BT_OP_Write_Local_Name            BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Local_Name)
#define BT_OP_Write_Scan_Enable           BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Scan_Enable)
#define BT_OP_Write_Page_Scan_Activity    BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Page_Scan_Activity)
#define BT_OP_Write_Inquiry_Scan_Activity BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Inquiry_Scan_Activity)
#define BT_OP_Write_Class_Of_Device       BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Class_Of_Device)
#define BT_OP_Write_Current_IAC_LAP       BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Current_IAC_LAP)
#define BT_OP_Write_Inquiry_Scan_Type     BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Inquiry_Scan_Type)
#define BT_OP_Write_Inquiry_Mode          BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Inquiry_Mode)
#define BT_OP_Write_Page_Scan_Type        BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Page_Scan_Type)
#define BT_OP_Write_EIR                   BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Extended_Inquiry_Rsp)
#define BT_OP_Write_Simple_Pairing_Mode   BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Write_Simple_Pairing_Mode)
#define BT_OP_Host_Buffer_Size            BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Host_Buffer_Size)
#define BT_OP_Set_Host_Flow_Control       BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Set_Controller_To_Host_Flow_Control)
#define BT_OP_Host_Num_Completed_Packets  BT_OPCODE(OGF_CTRL_BASEBAND, OCF_Host_Number_Of_Completed_Packets)
#define BT_OP_Write_Default_Link_Policy   BT_OPCODE(OGF_LINK_POLICY, OCF_Write_Default_Link_Policy_Settings)

#define BT_OP_Read_Local_Version          BT_OPCODE(OGF_INFO_PARAM, OCF_Read_Local_Version)
#define BT_OP_Read_Local_Features         BT_OPCODE(OGF_INFO_PARAM, OCF_Read_Local_Supported_Feat)
#define BT_OP_Read_Local_Ext_Features     BT_OPCODE(OGF_INFO_PARAM, OCF_Read_Local_Extended_Feat)
#define BT_OP_Read_Buffer_Size            BT_OPCODE(OGF_INFO_PARAM, OCF_Read_Buffer_Size)
#define BT_OP_Read_BD_ADDR                BT_OPCODE(OGF_INFO_PARAM, OCF_Read_BD_ADDR)
#define BT_OP_Read_Encryption_Key_Size    BT_OPCODE(OGF_STATUS_PARAM, OCF_Read_Encryption_Key_Size)

#define EVT_Inquiry_Complete             0x01u
#define EVT_Inquiry_Result               0x02u
#define EVT_Connection_Complete          0x03u
#define EVT_Connection_Request           0x04u
#define EVT_Disconnection_Complete       0x05u
#define EVT_Authentication_Complete      0x06u
#define EVT_Remote_Name_Request_Complete 0x07u
#define EVT_Encryption_Change            0x08u
#define EVT_Command_Complete             0x0Eu
#define EVT_Command_Status               0x0Fu
#define EVT_Hardware_Error               0x10u
#define EVT_Role_Change                  0x12u
#define EVT_Number_Of_Completed_Packets  0x13u
#define EVT_Mode_Change                  0x14u
#define EVT_PIN_Code_Request             0x16u
#define EVT_Link_Key_Request             0x17u
#define EVT_Link_Key_Notification        0x18u
#define EVT_Max_Slots_Change             0x1Bu
#define EVT_Inquiry_Result_With_RSSI     0x22u
#define EVT_Extended_Inquiry_Result      0x2Fu
#define EVT_IO_Capability_Request        0x31u
#define EVT_IO_Capability_Response       0x32u
#define EVT_User_Confirmation_Request    0x33u
#define EVT_User_Passkey_Request         0x34u
#define EVT_Simple_Pairing_Complete      0x36u
#define EVT_Link_Supervision_Timeout_Changed  0x38u
#define EVT_Link_Supervision_Timeout_Changed_Std 0x37u

#define BT_ERR_SUCCESS                   0x00u
#define BT_ERR_UNKNOWN_HCI_COMMAND       0x01u
#define BT_ERR_NO_CONNECTION             0x02u
#define BT_ERR_HARDWARE_FAILURE          0x03u
#define BT_ERR_PAGE_TIMEOUT              0x04u
#define BT_ERR_AUTHENTICATION_FAILURE    0x05u
#define BT_ERR_PIN_MISSING               0x06u
#define BT_ERR_MEMORY_CAPACITY           0x07u
#define BT_ERR_CONNECTION_TIMEOUT        0x08u
#define BT_ERR_MAX_NUM_OF_CONNS          0x09u
#define BT_ERR_COMMAND_DISALLOWED        0x0Cu
#define BT_ERR_REPEATED_ATTEMPTS         0x17u
#define BT_ERR_PAIRING_NOT_ALLOWED       0x18u
#define BT_ERR_UNIT_KEY_USED             0x1Cu
#define BT_ERR_INSUFFICIENT_SECURITY     0x1Fu
#define BT_ERR_CONNECTION_REJECTED_RESOURCES 0x0Fu

#define L2CAP_CID_SIGNALING   0x0001u

#define L2CAP_PSM_SDP          0x0001u
#define L2CAP_PSM_HID_CONTROL  0x0011u
#define L2CAP_PSM_HID_INTERRUPT 0x0013u

#define L2CAP_SIG_CMD_REJECT   0x01u
#define L2CAP_SIG_CONN_REQ     0x02u
#define L2CAP_SIG_CONN_RSP     0x03u
#define L2CAP_SIG_CONF_REQ     0x04u
#define L2CAP_SIG_CONF_RSP     0x05u
#define L2CAP_SIG_DISC_REQ     0x06u
#define L2CAP_SIG_DISC_RSP     0x07u
#define L2CAP_SIG_ECHO_REQ     0x08u
#define L2CAP_SIG_ECHO_RSP     0x09u
#define L2CAP_SIG_INFO_REQ     0x0Au
#define L2CAP_SIG_INFO_RSP     0x0Bu

#define L2CAP_CONN_SUCCESS         0x0000u
#define L2CAP_CONN_PENDING         0x0001u
#define L2CAP_CONN_PSM_NOT_SUPPORT 0x0002u
#define L2CAP_CONN_NO_RESOURCES    0x0004u

#define L2CAP_CONF_SUCCESS         0x0000u
#define L2CAP_CONF_UNACCEPT        0x0001u
#define L2CAP_CONF_REJECT          0x0002u

/* SDP PDU ID */
#define SDP_PDU_ERROR_RSP                  0x01u
#define SDP_PDU_SERVICE_SEARCH_REQ         0x02u
#define SDP_PDU_SERVICE_SEARCH_RSP         0x03u
#define SDP_PDU_SERVICE_ATTR_REQ           0x04u
#define SDP_PDU_SERVICE_ATTR_RSP           0x05u
#define SDP_PDU_SERVICE_SEARCH_ATTR_REQ    0x06u
#define SDP_PDU_SERVICE_SEARCH_ATTR_RSP    0x07u

#define BT_STATE_IDLE        0u   /* 上电初始化中 */
#define BT_STATE_READY       1u   /* 已可被发现/可连接 */
#define BT_STATE_CONNECTED   2u   /* 链路已建立 */
#define BT_STATE_HID_READY   3u   /* HID 通道已开，能发报告 */

/* 主循环里 bt_state 的取值，和上面一致，打印时可直接对照 */
#define BT_HANDLE_NONE       0xFFFFu

#define BT_RX_BUF_SIZE       1088u  /* 单包 H4 最大：ACL 1021 + 4 头 + 余量 */
#define BT_ACL_BUF_SIZE      1088u  /* ACL 重组缓冲 */
#define BT_TX_ACL_MAX        1024u

#define BT_CHAN_SDP          0u
#define BT_CHAN_HID_CTRL     1u
#define BT_CHAN_HID_INT      2u
#define BT_CHAN_COUNT        3u

typedef struct
{
    uint16_t psm;
    uint16_t lcid;        /* 本端 CID（0 表示未分配） */
    uint16_t rcid;        /* 对端 CID */
    uint8_t  used;        /* 已建立 */
    uint8_t  cfg_sent;    /* 我方已发过 CONF_REQ */
    uint8_t  cfg_rcvd;    /* 收到过对方的 CONF_REQ */
    uint8_t  open;        /* 双向配置完成 */
} bt_chan_t;

extern volatile uint8_t  g_bt_state;        /* BT_STATE_xxx */

/* 诊断用：最近一次断开事件的原因码（0xFF = 还没断过）。
 * 通过自定义 HID 报告 rep[4] 上报给上位机，便于排查
 * "电脑关掉蓝牙后设备没察觉链路已断" 这类问题。 */
extern volatile uint8_t  g_bt_last_disc_reason;

/* 诊断用：HID 控制/中断通道状态位图（bit0=控制通道 bit1=中断通道 bit2=可发报告） */
uint8_t bt_hid_chan_state(void);
extern volatile uint32_t g_bt_reports;      /* 已发出的键鼠报告数 */

/* 当前 HID 协议：0 = Boot（无 Report ID），1 = Report（带 Report ID）。
 * 由 HID Control 通道上的 SET_PROTOCOL 设置，发报告时按它换格式。 */
extern uint8_t           g_bt_hid_protocol;
extern volatile uint8_t  g_bt_error;        /* 0 = 正常，非 0 见 BT_STATE_xxx 注释 */

#define HIDP_TRANS_HANDSHAKE        0x0u
#define HIDP_TRANS_HID_CONTROL      0x1u
#define HIDP_TRANS_GET_REPORT       0x4u
#define HIDP_TRANS_SET_REPORT       0x5u
#define HIDP_TRANS_GET_PROTOCOL     0x6u
#define HIDP_TRANS_SET_PROTOCOL     0x7u
#define HIDP_TRANS_GET_IDLE         0x8u
#define HIDP_TRANS_SET_IDLE         0x9u
#define HIDP_TRANS_DATA             0xAu

#define HIDP_PROTOCOL_BOOT          0u
#define HIDP_PROTOCOL_REPORT        1u

#define HIDP_RPT_TYPE_OTHER         0u
#define HIDP_RPT_TYPE_INPUT         1u
#define HIDP_RPT_TYPE_OUTPUT        2u
#define HIDP_RPT_TYPE_FEATURE       3u
#define HIDP_RPT_SIZE_FLAG          0x8u

#define HIDP_HS_SUCCESS             0x00u
#define HIDP_HS_NOT_READY           0x01u
#define HIDP_HS_ERR_REPORT_ID       0x02u
#define HIDP_HS_ERR_UNSUPPORTED     0x03u
#define HIDP_HS_ERR_PARAMETER       0x04u

/* HIDP DATA 头（Interrupt 通道和 GET_REPORT 响应都用它）：
 *   0xA0 | 报告类型；A1 = 输入，A2 = 输出 */
#define HIDP_DATA_HDR(type)         (uint8_t)(0xA0u | ((type) & 0x0Fu))

extern uint16_t g_conn_handle;              /* 当前连接句柄 */
extern uint8_t  g_peer_addr[6];             /* 对端蓝牙地址（大端＝协议顺序） */
extern uint8_t  g_local_addr[6];            /* 本机蓝牙地址 */

extern uint16_t g_acl_mtu;                  /* 控制器单包 ACL 上限 */
extern uint16_t g_acl_pkts;                 /* 控制器 ACL 缓冲个数 */
extern volatile uint16_t g_cmd_credits;     /* 命令信用 */
extern volatile uint16_t g_acl_credits;     /* ACL 信用 */

extern uint8_t  g_link_key[16];             /* 保存的链路密钥 */
extern uint8_t  g_link_key_valid;

extern volatile uint32_t g_evt_count;       /* 收到的事件总数（判断链路是否活着） */

extern uint8_t  g_conn_by_us;

extern bt_chan_t g_chan[BT_CHAN_COUNT];

/* --- bt_hci.c：传输层 --- */
void     bt_transport_reset(void);
void     bt_transport_poll(void);                        /* 把串口收到的字节拆成包并分发 */
void     bt_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen);
void     bt_send_acl(uint16_t handle, uint16_t cid,
                     const uint8_t *payload, uint16_t len);
void     bt_flush_acl_credits(void);                     /* 归还信用 */

/* --- bt_cmd.c：初始化与应答命令 --- */
uint8_t  bt_init_sequence(void);                         /* 跑 18 条初始化命令 */
void     bt_cmd_accept(uint8_t role);
void     bt_cmd_reject(uint8_t reason);
void     bt_cmd_disconnect(uint16_t handle, uint8_t reason);   /* 主动断开链路 */
void     bt_cmd_link_key_reply(const uint8_t key[16]);
void     bt_cmd_link_key_negative(void);
void     bt_cmd_io_capability_reply(void);
void     bt_cmd_user_confirm_reply(void);
void     bt_cmd_auth_requested(void);
void     bt_cmd_set_encryption(void);
void     bt_cmd_scan_enable(uint8_t en);
void     bt_cmd_create_connection(const uint8_t addr[6]);
void     bt_cmd_set_supervision_timeout(void);   /* 连接建立后把监测超时提到 20s */

/* --- bt_evt.c：事件处理 --- */
void     bt_evt_dispatch(const uint8_t *params, uint8_t len);   /* params[0] = 事件码 */

void     bt_acl_input(const uint8_t *data, uint16_t len);       /* data[0] 起是 ACL 头 */
void     bt_l2cap_reset(void);
uint8_t  bt_hid_channel_ready(void);
void     bt_hid_send_report(const uint8_t *report, uint16_t len);
void     bt_l2cap_open_hid_control(void);     /* 回连时由本机主动发起 HID Control */
void     bt_l2cap_open_hid_interrupt(void);   /* 主机不开 Interrupt 时由我们自己发起 */

/* --- bt_gamepad.c：应用层（main.c 调用这三个） --- */
uint8_t  BT_Probe(void);                 /* 4 组串口参数自检，返回 1=链路通 */
void     BT_Init(void);                  /* 跑 18 条初始化命令 */
void     BT_Task(void);                  /* 主循环：收包 + 回连 + 发键鼠报告 */

void     bt_app_on_connected(void);      /* 链路建立后回调 */
void     bt_app_on_disconnected(void);   /* 断开后回调 */
void     bt_app_on_peer_connect(void);   /* 电脑主动发起连接，取消本机回连流程 */
void     bt_app_on_connect_failed(uint8_t status); /* 寻呼失败/命令拒绝后恢复扫描 */
void     bt_app_on_scan_enable_complete(uint8_t status); /* 分步回连：关扫描完成 */
void     bt_app_on_encrypted(void);      /* 加密完成 */
void     bt_app_on_link_key(const uint8_t addr[6], const uint8_t key[16]);
uint8_t  bt_app_link_key_for(const uint8_t addr[6], uint8_t out_key[16]);
void     bt_app_commit_bond(void);       /* 把暂存的 Key 写 Flash（空闲时调用） */
void     bt_app_drop_bond(void);         /* 作废绑定：擦 Flash + 允许重新配对 */

void     bt_app_set_keyboard_leds(uint8_t leds);
uint8_t  bt_app_get_keyboard_leds(void);

/* 自定义 ID3 输出报告，p[0] 是 Report ID，p[1..] 是 8 字节报告体。 */
void     bt_app_on_output_report(const uint8_t *p, uint16_t len);

/* 协议切换（SET_PROTOCOL）之后由 bt_l2cap 调用：
 * 清掉键盘/鼠标的"上次报告"缓存，下一轮扫描按新格式重发一次完整状态 */
void     bt_app_on_protocol_changed(void);

#endif /* __BT_HCI_H */

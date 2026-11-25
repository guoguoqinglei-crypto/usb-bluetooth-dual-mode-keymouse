#ifndef __HCI_COMMAND_H__
#define __HCI_COMMAND_H__

/* ============================================================================
 * hci_command.h —— HCI 命令层（沿用 ESP32_VHCI_demo/main/hci_command.h 的名字）
 * ----------------------------------------------------------------------------
 * 宏、opcode 编码方式、函数名都和 demo 保持一致：
 *   uart_send()   发一包完整的 H4 数据
 *   hci_send_cmd() 组装并发送一条 HCI 命令
 *   bt_Init()     demo 的初始化命令序列
 * 差别只在传输手段：demo 走 esp_vhci_cmd_print()，这里走 USART2。
 * ========================================================================== */

#include <stdint.h>

/* H4 传输层类型码 */
#define H4_TYPE_COMMAND   0x01
#define H4_TYPE_ACL       0x02
#define H4_TYPE_SCO       0x03
#define H4_TYPE_EVENT     0x04
#define H4_TYPE_ISO       0x05

/* 兼容原来的短名字 */
#define H4_TYPE_CMD   H4_TYPE_COMMAND

//===== HCI COMMAND OGF =======================================================
#define OGF_LINK_CTRL			    0x01
#define OGF_LINK_POLICY			    0x02
#define OGF_HC_N_BASEBAND		    0x03
#define OGF_INFO_PARAMS			    0x04
#define OGF_STATUS_PARAMS		    0x05
#define OGF_TESTING			        0x06
#define OGF_BLUETOOTH_LOGO_TESTING	0x3E
#define OGF_VENDOR_SPECIFIC		    0x3F

//===== HCI COMMAND OCF =======================================================
/* 只挑本工程用到的那些，命名与 demo 的 hci_command.h 完全一致 */
#define OCF_Reset						    0x003
#define OCF_Set_Event_Mask					0x001
#define OCF_Write_Local_Name				0x013
#define OCF_Write_Scan_Enable				0x01A
#define OCF_Write_Class_Of_Device			0x024
#define OCF_Write_Inquiry_Mode				0x045
#define OCF_Write_Inquiry_Scan_Activity		0x01E
#define OCF_Write_Inquiry_Scan_Type			0x043
#define OCF_Write_Page_Scan_Activity		0x01C
#define OCF_Write_Page_Scan_Type			0x047
#define OCF_Write_Current_IAC_LAP			0x03A
#define OCF_Write_Extended_Inquiry_Response	0x052
#define OCF_Write_Simple_Pairing_Mode		0x056
#define OCF_Host_Buffer_Size				0x033

#define OCF_Read_Local_Version_Information	0x001
#define OCF_Read_Local_Supported_Features	0x003
#define OCF_Read_Buffer_Size				0x005
#define OCF_Read_BD_ADDR					0x009

#define OCF_Write_Default_Link_Policy_Settings	0x00F

#define OCF_Create_Connection				0x005
#define OCF_Accept_Connection_Request		0x009
#define OCF_Reject_Connection_Request		0x00A
#define OCF_Link_Key_Request_Reply			0x00B
#define OCF_Link_Key_Request_Negative_Reply	0x00C
#define OCF_Authentication_Requested		0x011
#define OCF_Set_Connection_Encryption		0x013
#define OCF_IO_Capability_Request_Reply		0x02B
#define OCF_User_Confirmation_Request_Reply	0x02C

#define OCF_Read_Encryption_Key_Size		0x008

#define HCI_OPCODE(ogf, ocf)    ((uint16_t)((((uint16_t)(ogf)) << 10) | ((uint16_t)(ocf) & 0x03FFU)))

/* 老代码里那些 HCI_OP_xxx 名字保留成别名，免得改一堆调用点 */
#define HCI_OP_Reset                       HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Reset)
#define HCI_OP_Set_Event_Mask              HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Set_Event_Mask)
#define HCI_OP_Write_Local_Name            HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Local_Name)
#define HCI_OP_Write_Scan_Enable           HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Scan_Enable)
#define HCI_OP_Write_Class_Of_Device       HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Class_Of_Device)
#define HCI_OP_Write_Inquiry_Mode          HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Inquiry_Mode)
#define HCI_OP_Write_Inquiry_Scan_Activity HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Inquiry_Scan_Activity)
#define HCI_OP_Write_Inquiry_Scan_Type     HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Inquiry_Scan_Type)
#define HCI_OP_Write_Page_Scan_Activity    HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Page_Scan_Activity)
#define HCI_OP_Write_Page_Scan_Type        HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Page_Scan_Type)
#define HCI_OP_Write_Current_IAC_LAP       HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Current_IAC_LAP)
#define HCI_OP_Write_Extended_Inquiry_Rsp  HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Extended_Inquiry_Response)
#define HCI_OP_Write_Simple_Pairing_Mode   HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Write_Simple_Pairing_Mode)
#define HCI_OP_Host_Buffer_Size            HCI_OPCODE(OGF_HC_N_BASEBAND, OCF_Host_Buffer_Size)
#define HCI_OP_Read_Buffer_Size            HCI_OPCODE(OGF_INFO_PARAMS, OCF_Read_Buffer_Size)
#define HCI_OP_Read_BD_ADDR                HCI_OPCODE(OGF_INFO_PARAMS, OCF_Read_BD_ADDR)
#define HCI_OP_Write_Default_Link_Policy   HCI_OPCODE(OGF_LINK_POLICY, OCF_Write_Default_Link_Policy_Settings)
#define HCI_OP_Create_Connection           HCI_OPCODE(OGF_LINK_CTRL, OCF_Create_Connection)
#define HCI_OP_Accept_Connection_Request   HCI_OPCODE(OGF_LINK_CTRL, OCF_Accept_Connection_Request)
#define HCI_OP_Reject_Connection_Request   HCI_OPCODE(OGF_LINK_CTRL, OCF_Reject_Connection_Request)
#define HCI_OP_Link_Key_Request_Reply      HCI_OPCODE(OGF_LINK_CTRL, OCF_Link_Key_Request_Reply)
#define HCI_OP_Link_Key_Request_Neg_Reply  HCI_OPCODE(OGF_LINK_CTRL, OCF_Link_Key_Request_Negative_Reply)
#define HCI_OP_Authentication_Requested    HCI_OPCODE(OGF_LINK_CTRL, OCF_Authentication_Requested)
#define HCI_OP_Set_Connection_Encryption   HCI_OPCODE(OGF_LINK_CTRL, OCF_Set_Connection_Encryption)
#define HCI_OP_IO_Capability_Request_Reply HCI_OPCODE(OGF_LINK_CTRL, OCF_IO_Capability_Request_Reply)
#define HCI_OP_User_Confirmation_Reply     HCI_OPCODE(OGF_LINK_CTRL, OCF_User_Confirmation_Request_Reply)
#define HCI_OP_Read_Encryption_Key_Size    HCI_OPCODE(OGF_STATUS_PARAMS, OCF_Read_Encryption_Key_Size)

/* 发送一包完整的 H4 数据（命令/ACL 都可用） */
void uart_send(const uint8_t *buf, uint16_t len);

/* 组装并发送一条 HCI 命令：H4 + Opcode + Plen + Params */
uint8_t hci_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen);

/* 初始化：读版本/地址/缓冲、设事件掩码、写名称、开扫描（demo 的 bt_Init） */
void bt_Init(void);

/* 事件应答用的单发命令（demo 里是直接调 hci_send_cmd，这里包一层） */
uint8_t hci_write_scan_enable(uint8_t enable);
uint8_t hci_accept_connection(const uint8_t addr[6]);
uint8_t hci_reject_connection(const uint8_t addr[6]);
uint8_t hci_link_key_reply(const uint8_t addr[6], const uint8_t key[16]);
uint8_t hci_link_key_negative_reply(const uint8_t addr[6]);
uint8_t hci_io_capability_reply(const uint8_t addr[6]);
uint8_t hci_user_confirm_reply(const uint8_t addr[6]);
uint8_t hci_authentication_requested(uint16_t handle);
uint8_t hci_set_encryption(uint16_t handle);
uint8_t hci_read_encryption_key_size(uint16_t handle);
uint8_t hci_create_connection(const uint8_t addr[6]);

#endif

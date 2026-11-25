#ifndef __HCI_EVENT_H__
#define __HCI_EVENT_H__

/* ============================================================================
 * hci_event.h —— HCI 事件层（沿用 ESP32_VHCI_demo/main/hci_event.h 的名字）
 * ----------------------------------------------------------------------------
 * 事件码、错误码直接用 demo 的 HCI_xxx / ERR_xxx 命名；
 * 原来代码里的 EVT_xxx / BT_ERR_xxx 保留成别名，两套都能用。
 * ========================================================================== */

#include <stdint.h>

#define BDADDR_LEN 6

//===== HCI EVENT opcode ======================================================
#define HCI_Inquiry_Complete					        0x01
#define HCI_Inquiry_Result					            0x02
#define HCI_Connection_Complete					        0x03
#define HCI_Connection_Request					        0x04
#define HCI_Disconnection_Complete				        0x05
#define HCI_Authentication_Complete				        0x06
#define HCI_Remote_Name_Request_Complete		        0x07
#define HCI_Encryption_Change					        0x08
#define HCI_Command_Complete					        0x0E
#define HCI_Command_Status					            0x0F
#define HCI_Role_Change						            0x12
#define HCI_Number_Of_Completed_Packets				    0x13
#define HCI_Link_Key_Request					        0x17
#define HCI_Link_Key_Notification				        0x18
#define HCI_Max_Slots_Changed				            0x1B
#define HCI_IO_Capability_Request				        0x31
#define HCI_IO_Capability_Response				        0x32
#define HCI_User_Confirmation_Request				    0x33
#define HCI_Simple_Pairing_Complete				        0x36
#define HCI_Link_Supervision_Timeout_Changed			0x38

/* 兼容原来的短名字 */
#define EVT_Connection_Complete       HCI_Connection_Complete
#define EVT_Connection_Request        HCI_Connection_Request
#define EVT_Disconnection_Complete    HCI_Disconnection_Complete
#define EVT_Authentication_Complete   HCI_Authentication_Complete
#define EVT_Encryption_Change         HCI_Encryption_Change
#define EVT_Command_Complete          HCI_Command_Complete
#define EVT_Command_Status            HCI_Command_Status
#define EVT_Role_Change               HCI_Role_Change
#define EVT_Number_Of_Completed_Pkts  HCI_Number_Of_Completed_Packets
#define EVT_Max_Slots_Changed         HCI_Max_Slots_Changed
#define EVT_IO_Capability_Request     HCI_IO_Capability_Request
#define EVT_IO_Capability_Response    HCI_IO_Capability_Response
#define EVT_User_Confirmation_Request HCI_User_Confirmation_Request
#define EVT_Simple_Pairing_Complete   HCI_Simple_Pairing_Complete
#define EVT_Link_Key_Request          HCI_Link_Key_Request
#define EVT_Link_Key_Notification     HCI_Link_Key_Notification
#define EVT_Link_Supervision_Timeout  HCI_Link_Supervision_Timeout_Changed

//===== ERROR CODES ===========================================================
#define SUCCEED                             0
#define ERR_PAGE_TIMEOUT                    0x04
#define ERR_AUTH_FAILURE                    0x05
#define ERR_PIN_OR_KEY_MISSING              0x06
#define ERR_MAX_NUM_OF_CONNS                0x09
#define ERR_COMMAND_DISALLOWED              0x0C

/* 兼容原来的短名字 */
#define BT_ERR_OK                 0x00U
#define BT_ERR_PAGE_TIMEOUT       ERR_PAGE_TIMEOUT
#define BT_ERR_AUTH_FAILURE       ERR_AUTH_FAILURE
#define BT_ERR_PIN_OR_KEY_MISSING ERR_PIN_OR_KEY_MISSING
#define BT_ERR_MAX_NUM_OF_CONNS   ERR_MAX_NUM_OF_CONNS
#define BT_ERR_COMMAND_DISALLOWED ERR_COMMAND_DISALLOWED

/* data 指向 Event Code，长度包含 Event Code + Length + Parameters */
void hci_event_recv(const uint8_t *data, uint16_t len);

#endif

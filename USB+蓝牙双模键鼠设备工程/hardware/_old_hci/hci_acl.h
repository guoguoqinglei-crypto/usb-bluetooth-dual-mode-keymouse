#ifndef __HCI_ACL_H__
#define __HCI_ACL_H__

/* ============================================================================
 * hci_acl.h —— ACL / L2CAP / SDP（沿用 ESP32_VHCI_demo/main/hci_acl.h 的名字）
 * ----------------------------------------------------------------------------
 * L2CAP 信令 Code、PSM、三通道索引全部用 demo 的命名：
 *   CMD_REJ / CONN_REQ / CONN_RSP / CONF_REQ / CONF_RSP / DISC_REQ / DISC_RSP
 *   INFO_REQ / INFO_RSP、PSM_SDP / PSM_HID_Control / PSM_HID_Interrupt
 *   INDEX_SDP / INDEX_HID_CONTROL / INDEX_HID_INTERRUPT
 * 移植时用过的短名字保留成别名，调用点不用全改。
 * ========================================================================== */

#include <stdint.h>

/* L2CAP 固定信令 CID */
#define L2CAP_CID_SIGNALING     0x0001

/* PSM */
#define PSM_SDP                 0x0001
#define PSM_RFCOMM              0x0003
#define PSM_HID_Control         0x0011
#define PSM_HID_Interrupt       0x0013
#define PSM_UNKNOW              0x00FF

/* L2CAP 信令 Code */
#define CMD_REJ                 0x01
#define CONN_REQ                0x02
#define CONN_RSP                0x03
#define CONF_REQ                0x04
#define CONF_RSP                0x05
#define DISC_REQ                0x06
#define DISC_RSP                0x07
#define ECHO_REQ                0x08
#define ECHO_RSP                0x09
#define INFO_REQ                0x0A
#define INFO_RSP                0x0B

/* L2CAP Connection Response Result */
#define CONNECTION_SUCCESSFUL   0x0000
#define CONNECTION_PENDING      0x0001
#define PSM_NOT_SUPPORTED       0x0002
#define SECURITY_BLOCK          0x0003
#define NO_RESOURCES_AVAILABLE  0x0004

/* SDP PDU ID（只保留本工程用到的两个） */
#define SERVICESEARCHATTR_REQ   0x06
#define SERVICESEARCHATTR_RSP   0x07

/* L2CAP 逻辑通道索引：SDP、HID Control、HID Interrupt 三个槽位 */
#define INDEX_SDP               0
#define INDEX_HID_CONTROL       1
#define INDEX_HID_INTERRUPT     2

/* ACL 数据包 PB 标志 */
#define ACL_PB_CONTINUATION     0x1000
#define ACL_PB_START            0x2000

/* ---- 兼容移植时用过的短名字 ---- */
#define CID_SIGNALING           L2CAP_CID_SIGNALING
#define PSM_HID_CTRL            PSM_HID_Control
#define PSM_HID_INT             PSM_HID_Interrupt
#define SIG_CONN_REQ            CONN_REQ
#define SIG_CONN_RSP            CONN_RSP
#define SIG_CONF_REQ            CONF_REQ
#define SIG_CONF_RSP            CONF_RSP
#define SIG_DISC_REQ            DISC_REQ
#define SIG_DISC_RSP            DISC_RSP
#define SIG_INFO_REQ            INFO_REQ
#define SIG_INFO_RSP            INFO_RSP
#define SDP_SEARCH_ATTR_REQ     SERVICESEARCHATTR_REQ
#define SDP_SEARCH_ATTR_RSP     SERVICESEARCHATTR_RSP
#define RSP_PSM_NOT_SUPPORTED   PSM_NOT_SUPPORTED
#define RSP_NO_RESOURCES        NO_RESOURCES_AVAILABLE

/* data 为 ACL 负载（不含 HCI 包头），pb 为 Packet_Boundary_Flag（0..3） */
void LMacl_receive(uint8_t pb, const uint8_t *data, uint16_t len);

/* 发一包 ACL 数据，超过控制器 MTU 时自动分片 */
uint8_t LMacl_send(uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len);

/* HID 输入报告：data = 0xA1 + 报告内容 */
void    l2cap_send_hid_input(const uint8_t *data, uint16_t len);
uint8_t l2cap_hid_ready(void);       /* HID Interrupt 通道是否已打开 */
void    l2cap_reset(void);           /* 断线时清通道表 */
void    l2cap_open_hid(void);        /* 回连时由本机主动建通道 */

#endif

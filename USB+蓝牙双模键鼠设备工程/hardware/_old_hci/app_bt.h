#ifndef __APP_BT_H__
#define __APP_BT_H__

/* ============================================================================
 * app_bt.h —— 传输层与全局状态（沿用 ESP32_VHCI_demo/main/app_bt.h 的名字）
 * ----------------------------------------------------------------------------
 * 移植关系（文件名、函数名都按 demo 原样）：
 *   ESP32_VHCI_demo/main/app_bt.c      -> app_bt.c      H4 收发与分包分发
 *   ESP32_VHCI_demo/main/hci_command.c -> hci_command.c 命令组装 + bt_Init()
 *   ESP32_VHCI_demo/main/hci_event.c   -> hci_event.c   事件分发
 *   ESP32_VHCI_demo/main/hci_acl.c     -> hci_acl.c     ACL 重组 + L2CAP + SDP
 *   厂商参考 Init_rda5876.c            -> Init_rda5876.c 上电时序 + 补丁下载
 *   新增：bt_uart.c（USART2 底层）、bt_input.c（手柄报告 / 绑定 / 回连）
 *
 * 与 ESP32 最大的不同：
 *   ESP32 的 VHCI 一次给一整个包（带 0x01/0x02/0x04 指示符），
 *   STM32 这边是一串连续字节流，所以必须先做 H4 拆包。
 * ========================================================================== */

#include <stdint.h>

#include "hci_command.h"
#include "hci_event.h"
#include "hci_acl.h"

/* ---------------- 全局状态（Keil 调试器可直接 Watch） ---------------- */
#define BT_HANDLE_NONE  0xFFFFU

extern volatile uint16_t conn_handle;      /* 连接句柄，未连接 = 0xFFFF */
extern uint8_t           peer_bdaddr[6];   /* 对端（电脑）蓝牙地址，HCI 字节序 */
extern uint8_t           link_key[16];     /* 配对产生的 Link Key，回连靠它 */
extern volatile uint8_t  link_key_valid;   /* 0 = 没有可用的 Key */
extern volatile uint16_t acl_mtu;          /* 控制器单包 ACL 上限，SDP 大包按它分片 */
extern volatile uint16_t acl_pkts;         /* 控制器 ACL 缓冲个数 */
extern volatile uint32_t hci_cmd_count;    /* 已发出命令条数 */
extern volatile uint32_t hci_rx_events;    /* 已收到事件包数 */
extern volatile uint16_t hci_last_event;   /* 最近一个事件码 */
extern volatile uint8_t  hci_last_status;  /* 最近一条命令的状态字节 */

/* 应用状态：0=未初始化 1=初始化中 2=可被发现（等连接） 3=已连接 4=HID 通道就绪 */
extern volatile uint8_t  bt_state;
extern volatile uint32_t bt_reports;       /* 已发出的手柄报告数 */

/* 初始化结果：0 = 正常。非 0 时日志里会打出 rda_stage，
 * 用来看卡在哪一步。 */
#define BT_STATE_OK       0U
#define BT_STATE_NO_CTRL  1U   /* 命令发完了，控制器一个事件都没回 */
extern volatile uint8_t  bt_err;

/* ---------------- 传输层 ---------------- */
void    bt_transport_init(void);   /* 清 H4 状态机 + 冲掉串口残留 */
void    bt_transport_poll(void);   /* 主循环周期调用：拆包并分发 */

/* 串口自检：在 4 组候选参数（921600 8E1/8N1、115200 8N1/8E1）上各发一条
 * 只读的 Read_Local_Version，返回 1 表示找到能通的一组且已设好，
 * 返回 0 表示全都没回音（问题在硬件侧）。只在 Init_rda5876() 成功之后调一次。 */
uint8_t bt_link_probe(void);

/* ---------------- 应用层（bt_input.c） ---------------- */
void bt_input_init(void);          /* Init_rda5876() 成功后调用 */
void bt_input_task(void);          /* 主循环调用，不得阻塞 */

/* 事件层回调到应用层的钩子 */
void    bt_input_save_bond(const uint8_t addr[6], const uint8_t key[16]);
void    bt_input_request_bond_save(void);
uint8_t bt_input_is_initiator(void);
void    bt_input_on_connected(void);
void    bt_input_on_disconnected(void);
void    bt_input_on_peer_connect(void);

/* 毫秒时基（底层是 usb_audio.c 的 TIM4 计数） */
uint32_t hal_time_ms(void);

#endif

#ifndef __RDA_INIT_H
#define __RDA_INIT_H

#include <stdint.h>

#define RDA_OK           0U

/* 卡在第几步（日志里的 stage=N 就是这个值） */
extern volatile uint8_t rda_stage;

/* 上电 + 灌补丁 + 切到 921600 8E1。
 * 返回 RDA_OK 表示流程走完（不代表芯片一定应答，应答由 HCI 层的 HCI_Reset 验证） */
uint8_t Init_rda5876(void);

#endif

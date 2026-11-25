#ifndef _KEY_H
#define _KEY_H

#include <stdint.h>
#include <stm32f10x.h>

// 按键引脚定义
#define KEY_A_PIN   GPIO_Pin_4    // PC4 - A键 (IO1)
#define KEY_B_PIN   GPIO_Pin_5    // PC5 - B键 (IO2)
#define KEY_X_PIN   GPIO_Pin_0    // PB0 - X键 (IO3)
#define KEY_Y_PIN   GPIO_Pin_1    // PB1 - Y键 (IO4)
#define KEY_UP_PIN  GPIO_Pin_2    // PB2 - UP键 (IO5)
#define KEY_DN_PIN  GPIO_Pin_10   // PB10 - DN键 (IO6)
#define KEY_LF_PIN  GPIO_Pin_11   // PB11 - LF键 (IO7)
#define KEY_RG_PIN  GPIO_Pin_14   // PB14 - RG键 (IO8)

#define KEY_A_PORT  GPIOC
#define KEY_B_PORT  GPIOC
#define KEY_X_PORT  GPIOB
#define KEY_Y_PORT  GPIOB
#define KEY_UP_PORT GPIOB
#define KEY_DN_PORT GPIOB
#define KEY_LF_PORT GPIOB
#define KEY_RG_PORT GPIOB

// 按键编号
#define KEY_A   1
#define KEY_B   2
#define KEY_X   3
#define KEY_Y   4
#define KEY_UP  5
#define KEY_DN  6
#define KEY_LF  7
#define KEY_RG  8

// 按键回调函数类型
typedef void (*Key_Callback)(uint8_t key);

void Key_Init(void);
void Key_Interrupt_Init(void);
void Key_SetCallback(Key_Callback callback);
uint8_t Key_Scan(uint8_t mode);
uint8_t Key_GetState(uint8_t key);

/* ========== 按键状态机 (FSM) 接口 ========== */

// 按键状态
#define KEY_STATE_IDLE        0   // 空闲：未按下
#define KEY_STATE_DEBOUNCE    1   // 消抖：首次检测到低电平
#define KEY_STATE_PRESSED     2   // 确认按下
#define KEY_STATE_LONGPRESS   3   // 长按保持

// 按键事件
#define KEY_EVENT_NONE        0   // 无事件
#define KEY_EVENT_PRESS       1   // 短按按下事件
#define KEY_EVENT_RELEASE     2   // 释放事件
#define KEY_EVENT_LONG_PRESS  3   // 长按事件

// 状态机参数（基于 10ms 扫描周期）
#define KEY_FSM_SCAN_INTERVAL_MS  10  // 建议扫描周期，单位 ms
#define KEY_FSM_DEBOUNCE_CNT      2   // 消抖计数：2 * 10ms = 20ms（50ms 对鼠标点击太迟钝）
#define KEY_FSM_LONG_PRESS_CNT    30  // 长按计数：30 * 10ms = 300ms（调小便于验证，稳定后建议改回 100~200）

// 单键状态机结构体
typedef struct
{
    uint8_t  state;     // 当前状态
    uint16_t cnt;       // 状态计数器
    uint8_t  event;     // 当前产生的事件（读取后自动清除）
} KeyFSM_TypeDef;

void Key_FSM_Init(void);
void Key_FSM_Update(void);
uint8_t Key_FSM_GetEvent(uint8_t key);

/* 消抖后的稳定按键状态：1 = 按下，0 = 松开。
 * 上层（键鼠扫描、自定义 HID 位图、蓝牙按键位图）统一用这个接口，
 * 不再直接读 Key_GetState() 的裸电平，避免机械抖动被当成多次按键。 */
uint8_t Key_IsDown(uint8_t key);

#endif

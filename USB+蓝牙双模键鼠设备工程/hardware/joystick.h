#ifndef _JOYSTICK_H
#define _JOYSTICK_H

#include <stdint.h>
#include <stm32f10x.h>

/* 模拟轴：左摇杆 X/Y、右摇杆 X/Y */
#define JOYSTICK_LX  0
#define JOYSTICK_LY  1
#define JOYSTICK_RX  2
#define JOYSTICK_RY  3

/* 摇杆按下按键 */
#define JOYSTICK_L3  0
#define JOYSTICK_R3  1

/* 8 方向编码 */
#define JOY_DIR_CENTER      0   // 中位
#define JOY_DIR_UP          1   // 上
#define JOY_DIR_UP_RIGHT    2   // 右上
#define JOY_DIR_RIGHT       3   // 右
#define JOY_DIR_DOWN_RIGHT  4   // 右下
#define JOY_DIR_DOWN        5   // 下
#define JOY_DIR_DOWN_LEFT   6   // 左下
#define JOY_DIR_LEFT        7   // 左
#define JOY_DIR_UP_LEFT     8   // 左上

typedef struct
{
    // X 轴编号
    uint8_t xAxis;
    // Y 轴编号
    uint8_t yAxis;
    // 滤波后的 X 轴 ADC 值
    uint16_t x;
    // 滤波后的 Y 轴 ADC 值
    uint16_t y;
    // 当前 8 方向编号
    uint8_t direction;
    // 当前向上亮度，范围 0~100
    uint8_t upBrightness;
    // 状态是否已经初始化
    uint8_t initialized;
} Joystick_State;

// 初始化两个摇杆和 ADC
void Joystick_Init(void);

/* 静止时自动重新标定中位（消除摇杆漂移导致光标自己移动）。
 * 由 1ms 周期任务调用，内部约 100ms 生效一次。 */
void Joystick_AutoRecenter(void);

/* 立即重新标定中位（快速，约 160us）。双模切换后调用，补偿蓝牙射频造成的静止点偏移。 */
void Joystick_RecalibrateNow(void);
// 读取指定摇杆轴的 ADC 原始值
uint16_t Joystick_ReadAxis(uint8_t axis);

// 把原始 ADC 值按校准中点转换为 -32767~32767
int16_t Joystick_MapRawValue(uint8_t axis, uint16_t rawValue);

/* Joystick_Init() 执行期间保持两个摇杆在中位，以便校准中点 */
void Joystick_StateInit(Joystick_State *state, uint8_t xAxis, uint8_t yAxis);
void Joystick_Update(Joystick_State *state);

/* 摇杆按键原始电平：按下返回 0，松开返回 1。仅供状态机采样使用。 */
uint8_t Joystick_GetKeyState(uint8_t key);

/* L3/R3 独立状态机：10ms 调用一次，20ms 按下/释放消抖。
 * USB 与蓝牙任务只读取 Joystick_KeyIsDown()，不得各自重复更新状态机。 */
void Joystick_KeyInit(void);
void Joystick_KeyUpdate(void);
uint8_t Joystick_KeyIsDown(uint8_t key);

#endif

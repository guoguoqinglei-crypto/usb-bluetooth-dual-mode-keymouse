#ifndef KM_CONFIG_H
#define KM_CONFIG_H

#include <stdint.h>

/* USB 自定义接口的无 Report ID Feature Report，固定 32 字节。 */
#define KM_CONFIG_SIZE             32u
#define KM_CONFIG_MAGIC            0x4Bu
#define KM_CONFIG_VERSION          1u
#define KM_CONFIG_CMD_SAVE         1u
#define KM_CONFIG_CMD_DEFAULT      2u
#define KM_CONFIG_STATUS_IDLE      0u
#define KM_CONFIG_STATUS_PENDING   1u
#define KM_CONFIG_STATUS_SAVED     2u
#define KM_CONFIG_STATUS_ERROR     3u

/* 可改键对象：RG 专用于 USB/蓝牙切换，不在此列表中。 */
enum
{
    KM_KEY_A = 0, KM_KEY_B, KM_KEY_X, KM_KEY_Y, KM_KEY_UP,
    KM_KEY_DN, KM_KEY_LF, KM_KEY_L3, KM_KEY_R3, KM_KEY_COUNT
};

#define KM_ACTION_NONE             0x0000u
#define KM_ACTION_KEYBOARD(code)   ((uint16_t)(0x0100u | ((code) & 0xFFu)))
#define KM_ACTION_MODIFIER(mask)   ((uint16_t)(0x0200u | ((mask) & 0xFFu)))
#define KM_ACTION_MOUSE(mask)      ((uint16_t)(0x0300u | ((mask) & 0xFFu)))
#define KM_ACTION_TYPE(action)     ((uint16_t)(action) & 0xFF00u)
#define KM_ACTION_VALUE(action)    ((uint8_t)((action) & 0xFFu))

void KMConfig_Init(void);
void KMConfig_Process(void);
uint8_t KMConfig_SetFeature(const uint8_t *data, uint16_t len);
void KMConfig_GetFeature(uint8_t *data, uint16_t len);

uint16_t KMConfig_Action(uint8_t key);
uint8_t KMConfig_IsDefaultMapping(void);
uint8_t KMConfig_BuildKeyboard(uint8_t bm, uint8_t joy, uint8_t keys[6]);
uint8_t KMConfig_BuildMouse(uint8_t bm, uint8_t joy);

int8_t KMConfig_AxisToMove(int32_t value);
int8_t KMConfig_Wheel(int32_t value);

#endif /* KM_CONFIG_H */

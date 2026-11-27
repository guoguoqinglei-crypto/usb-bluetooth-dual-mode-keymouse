#include "km_config.h"

#include <string.h>
#include <stm32f10x.h>
#include <FreeRTOS.h>
#include <task.h>

/* 0x0803F000/0x0803F800 已分别用于模式记忆和蓝牙配对。
 * 当前固件约 80KB，配置使用 0x0803D000/0x0803E000 两个独立 2KB 页。 */
#define CFG_SLOT_ADDR_0       0x0803D000u
#define CFG_SLOT_ADDR_1       0x0803E000u
#define CFG_RECORD_MAGIC      0x4B4D4347u   /* "KMCG" */

typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint8_t  data[KM_CONFIG_SIZE];
    uint32_t crc;
} km_record_t;

static uint8_t s_data[KM_CONFIG_SIZE];
static volatile uint8_t s_savePending;
static uint8_t s_status = KM_CONFIG_STATUS_IDLE;
static uint8_t s_activeSlot = 0xFFu;
static uint32_t s_sequence;

static const uint16_t s_defaultActions[KM_KEY_COUNT] =
{
    KM_ACTION_MOUSE(0x01u),       /* A：鼠标左键 */
    KM_ACTION_MOUSE(0x02u),       /* B：鼠标右键 */
    KM_ACTION_KEYBOARD(0x39u),    /* X：CapsLock */
    KM_ACTION_KEYBOARD(0x53u),    /* Y：NumLock */
    KM_ACTION_KEYBOARD(0x2Cu),    /* UP：Space */
    KM_ACTION_KEYBOARD(0x28u),    /* DN：Enter */
    KM_ACTION_MOUSE(0x04u),       /* LF：鼠标中键 */
    KM_ACTION_MODIFIER(0x02u),    /* L3：左 Shift */
    KM_ACTION_MODIFIER(0x01u)     /* R3：左 Ctrl */
};

static uint32_t crc32(const uint8_t *data, uint16_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint16_t i;
    uint8_t bit;

    for (i = 0u; i < len; ++i)
    {
        crc ^= data[i];
        for (bit = 0u; bit < 8u; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void payload_crc(uint8_t *data)
{
    wr32(&data[28], 0u);
    wr32(&data[28], crc32(data, 28u));
}

static void set_defaults(void)
{
    uint8_t i;

    memset(s_data, 0, sizeof(s_data));
    s_data[0] = KM_CONFIG_MAGIC;
    s_data[1] = KM_CONFIG_VERSION;
    for (i = 0u; i < KM_KEY_COUNT; ++i)
    {
        wr16(&s_data[4u + i * 2u], s_defaultActions[i]);
    }
    s_data[22] = 100u;               /* 左摇杆灵敏度 100% */
    s_data[23] = 100u;               /* 右摇杆灵敏度 100% */
    wr16(&s_data[24], 9000u);        /* 左摇杆死区，与原鼠标路径一致 */
    wr16(&s_data[26], 12000u);       /* 右摇杆滚轮阈值，与原行为一致 */
    payload_crc(s_data);
}

static uint8_t payload_valid(const uint8_t *data)
{
    uint8_t i;

    if ((data[0] != KM_CONFIG_MAGIC) || (data[1] != KM_CONFIG_VERSION) ||
        (rd32(&data[28]) != crc32(data, 28u)))
    {
        return 0u;
    }
    for (i = 0u; i < KM_KEY_COUNT; ++i)
    {
        const uint16_t action = rd16(&data[4u + i * 2u]);
        const uint16_t type = KM_ACTION_TYPE(action);
        const uint8_t value = KM_ACTION_VALUE(action);

        if ((action == KM_ACTION_NONE) || (type == 0x0100u))
        {
            continue;                /* 无动作或普通键盘 Usage（0 也留给无动作） */
        }
        if ((type == 0x0200u) && (value <= 0xFFu))
        {
            continue;
        }
        if ((type == 0x0300u) && ((value & ~0x07u) == 0u))
        {
            continue;
        }
        return 0u;
    }
    /* 灵敏度 25~200%，死区不超过约 73% 满量程。 */
    if ((data[22] < 25u) || (data[22] > 200u) ||
        (data[23] < 25u) || (data[23] > 200u) ||
        (rd16(&data[24]) > 24000u) || (rd16(&data[26]) > 24000u))
    {
        return 0u;
    }
    return 1u;
}

static uint8_t record_valid(const km_record_t *record)
{
    uint32_t crc;

    if (record->magic != CFG_RECORD_MAGIC)
    {
        return 0u;
    }
    crc = crc32((const uint8_t *)&record->sequence,
                (uint16_t)(sizeof(record->sequence) + KM_CONFIG_SIZE));
    if ((record->crc != crc) || (payload_valid(record->data) == 0u))
    {
        return 0u;
    }
    return 1u;
}

void KMConfig_Init(void)
{
    const km_record_t *a = (const km_record_t *)CFG_SLOT_ADDR_0;
    const km_record_t *b = (const km_record_t *)CFG_SLOT_ADDR_1;
    const uint8_t validA = record_valid(a);
    const uint8_t validB = record_valid(b);
    const km_record_t *selected = 0;

    set_defaults();
    if ((validA != 0u) && (validB != 0u))
    {
        selected = ((int32_t)(a->sequence - b->sequence) >= 0) ? a : b;
    }
    else if (validA != 0u) { selected = a; }
    else if (validB != 0u) { selected = b; }

    if (selected != 0)
    {
        memcpy(s_data, selected->data, KM_CONFIG_SIZE);
        s_data[2] = 0u;
        s_data[3] = KM_CONFIG_STATUS_SAVED;
        payload_crc(s_data);
        s_status = KM_CONFIG_STATUS_SAVED;
        s_sequence = selected->sequence;
        s_activeSlot = (selected == a) ? 0u : 1u;
    }
}

uint8_t KMConfig_SetFeature(const uint8_t *data, uint16_t len)
{
    if ((data == 0) || (len < KM_CONFIG_SIZE) || (payload_valid(data) == 0u) ||
        ((data[2] != KM_CONFIG_CMD_SAVE) && (data[2] != KM_CONFIG_CMD_DEFAULT)))
    {
        return 0u;
    }

    if (data[2] == KM_CONFIG_CMD_DEFAULT)
    {
        set_defaults();
    }
    else
    {
        memcpy(s_data, data, KM_CONFIG_SIZE);
        s_data[2] = 0u;
        payload_crc(s_data);
    }
    s_status = KM_CONFIG_STATUS_PENDING;
    s_data[3] = KM_CONFIG_STATUS_PENDING;
    payload_crc(s_data);
    s_savePending = 1u;
    return 1u;                        /* Flash 擦写由 KMConfig_Process 异步完成 */
}

void KMConfig_GetFeature(uint8_t *data, uint16_t len)
{
    if ((data == 0) || (len < KM_CONFIG_SIZE))
    {
        return;
    }
    memcpy(data, s_data, KM_CONFIG_SIZE);
    data[2] = 0u;
    data[3] = s_status;
    payload_crc(data);
}

void KMConfig_Process(void)
{
    km_record_t record;
    const uint32_t address = (s_activeSlot == 0u) ? CFG_SLOT_ADDR_1 : CFG_SLOT_ADDR_0;
    const uint8_t newSlot = (s_activeSlot == 0u) ? 1u : 0u;
    uint32_t i;
    const uint32_t *words = (const uint32_t *)&record;
    uint8_t ok = 1u;

    if (s_savePending == 0u)
    {
        return;
    }
    s_savePending = 0u;

    memset(&record, 0xFF, sizeof(record));
    record.magic = CFG_RECORD_MAGIC;
    record.sequence = s_sequence + 1u;
    memcpy(record.data, s_data, KM_CONFIG_SIZE);
    record.data[2] = 0u;
    record.data[3] = KM_CONFIG_STATUS_SAVED;
    payload_crc(record.data);
    record.crc = crc32((const uint8_t *)&record.sequence,
                       (uint16_t)(sizeof(record.sequence) + KM_CONFIG_SIZE));

    taskENTER_CRITICAL();             /* 防止与蓝牙配对页/模式页的 Flash 操作交叉 */
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    if (FLASH_ErasePage(address) != FLASH_COMPLETE) { ok = 0u; }
    for (i = 0u; (ok != 0u) && (i < sizeof(record) / 4u); ++i)
    {
        if (FLASH_ProgramWord(address + i * 4u, words[i]) != FLASH_COMPLETE) { ok = 0u; }
    }
    FLASH_Lock();
    taskEXIT_CRITICAL();

    if ((ok != 0u) && (memcmp((const void *)address, &record, sizeof(record)) == 0))
    {
        s_sequence = record.sequence;
        s_activeSlot = newSlot;
        s_status = KM_CONFIG_STATUS_SAVED;
    }
    else
    {
        s_status = KM_CONFIG_STATUS_ERROR; /* RAM 配置仍有效，可重新点击保存 */
    }
    s_data[3] = s_status;
    payload_crc(s_data);
}

uint16_t KMConfig_Action(uint8_t key)
{
    return (key < KM_KEY_COUNT) ? rd16(&s_data[4u + key * 2u]) : KM_ACTION_NONE;
}

uint8_t KMConfig_IsDefaultMapping(void)
{
    uint8_t i;
    for (i = 0u; i < KM_KEY_COUNT; ++i)
    {
        if (KMConfig_Action(i) != s_defaultActions[i]) { return 0u; }
    }
    return 1u;
}

static uint8_t key_down(uint8_t key, uint8_t bm, uint8_t joy)
{
    if (key <= KM_KEY_LF)
    {
        static const uint8_t bits[KM_KEY_LF + 1u] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u };
        return (bm & (uint8_t)(1u << bits[key])) ? 1u : 0u;
    }
    return (joy & (uint8_t)(1u << (key - KM_KEY_L3))) ? 1u : 0u;
}

uint8_t KMConfig_BuildKeyboard(uint8_t bm, uint8_t joy, uint8_t keys[6])
{
    uint8_t modifier = 0u;
    uint8_t count = 0u;
    uint8_t i;

    memset(keys, 0, 6u);
    for (i = 0u; i < KM_KEY_COUNT; ++i)
    {
        const uint16_t action = KMConfig_Action(i);
        const uint16_t type = KM_ACTION_TYPE(action);
        const uint8_t value = KM_ACTION_VALUE(action);

        if (key_down(i, bm, joy) == 0u) { continue; }
        if (type == 0x0200u) { modifier |= value; }
        else if ((type == 0x0100u) && (value != 0u) && (count < 6u)) { keys[count++] = value; }
    }
    return modifier;
}

uint8_t KMConfig_BuildMouse(uint8_t bm, uint8_t joy)
{
    uint8_t buttons = 0u;
    uint8_t i;

    for (i = 0u; i < KM_KEY_COUNT; ++i)
    {
        const uint16_t action = KMConfig_Action(i);
        if ((KM_ACTION_TYPE(action) == 0x0300u) && (key_down(i, bm, joy) != 0u))
        {
            buttons |= KM_ACTION_VALUE(action);
        }
    }
    return buttons;
}

int8_t KMConfig_AxisToMove(int32_t value)
{
    const int32_t deadzone = rd16(&s_data[24]);
    const int32_t maximum = (12 * s_data[22]) / 100;
    int32_t magnitude = (value < 0) ? -value : value;
    int32_t range = 32767 - deadzone;
    int32_t move;

    if ((magnitude <= deadzone) || (range <= 0)) { return 0; }
    magnitude -= deadzone;
    move = (int32_t)(((int64_t)magnitude * magnitude * maximum + (int64_t)range * range / 2) /
                     ((int64_t)range * range));
    if (move < 1) { move = 1; }
    if (move > maximum) { move = maximum; }
    return (value < 0) ? (int8_t)-move : (int8_t)move;
}

int8_t KMConfig_Wheel(int32_t value)
{
    int32_t threshold = ((int32_t)rd16(&s_data[26]) * 100) / s_data[23];
    if (threshold < 500) { threshold = 500; }
    if (threshold > 30000) { threshold = 30000; }
    if (value > threshold) { return 1; }
    if (value < -threshold) { return -1; }
    return 0;
}

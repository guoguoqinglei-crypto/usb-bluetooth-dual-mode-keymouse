#include <stdint.h>
#include <string.h>

#include <stm32f10x.h>

#include <tusb.h>
#include <device/dcd.h>

#include <usb_hid.h>
#include <usb_descriptors.h>

#include <key.h>
#include <joystick.h>
#include <tim2.h>
#include <uart.h>
#include <uart.h>

static const uint8_t desc_kbd[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

static const uint8_t desc_mouse[] = { TUD_HID_REPORT_DESC_MOUSE() };

static const uint8_t desc_custom[] =
{
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x09, 0x01,
    0x95, 0x09,                 /* 输入 9B：新增 rep[8] = L3/R3 状态 */
    0x81, 0x02,
    0x09, 0x02,
    0x95, 0x08,                 /* 输出仍 8B，保持 RGB 命令协议兼容 */
    0x91, 0x02,
    0xC0
};

/* 编译期对账：描述符实际长度必须等于 usb_descriptors.c 里写进
 * wDescriptorLength 的值。改描述符忘了改长度，会在这里直接编译报错。 */
TU_VERIFY_STATIC(sizeof(desc_kbd)    == HID_DESC_LEN_KEYBOARD, "keyboard report desc length mismatch");
TU_VERIFY_STATIC(sizeof(desc_mouse)  == HID_DESC_LEN_MOUSE,    "mouse report desc length mismatch");
TU_VERIFY_STATIC(sizeof(desc_custom) == HID_DESC_LEN_CUSTOM,   "custom report desc length mismatch");

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance)
    {
        case USB_HID_INST_KEYBOARD: return desc_kbd;
        case USB_HID_INST_MOUSE:    return desc_mouse;
        case USB_HID_INST_CUSTOM:   return desc_custom;
        default:                    return NULL;
    }
}

/* TinyUSB 在无 RTOS 模式下要求应用提供毫秒时基 */
static volatile uint32_t s_millis = 0U;

uint32_t tusb_time_millis_api(void)
{
    return s_millis;
}

uint32_t hal_time_ms(void)
{
    return s_millis;
}

/* USB 枚举完成（主机已配置设备）返回 1。
 * 蓝牙双模已移除，保留这个查询接口给上层 / 调试用。 */
uint8_t UsbHid_IsMounted(void)
{
    return tud_mounted() ? 1U : 0U;
}

void TIM4_IRQHandler(void)
{
    if ((TIM4->SR & TIM_SR_UIF) != 0U)
    {
        TIM4->SR = (uint16_t)~TIM_SR_UIF;
        s_millis++;
    }
}

static uint8_t s_timebase_started = 0U;

void UsbHid_TimeBaseInit(void)
{
    if (s_timebase_started != 0U)
    {
        return;
    }
    s_timebase_started = 1U;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    TIM4->PSC = 72U - 1U;
    TIM4->ARR = 1000U - 1U;
    TIM4->CNT = 0U;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->SR  = 0U;

    TIM4->DIER |= TIM_DIER_UIE;
    TIM4->CR1  |= TIM_CR1_CEN;

    NVIC_SetPriority(TIM4_IRQn, 3);
    NVIC_EnableIRQ(TIM4_IRQn);
}

void UsbHid_PreInit(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(GPIOA, GPIO_Pin_12);
}

/* ============================================================================
 * 长按 RG 键切换 USB / 蓝牙模式
 * ----------------------------------------------------------------------------
 * RG 键（PB14）本来没有分配给任何键盘或鼠标功能，正好用来做模式切换：
 *   默认「自动」：插着 USB 走 USB，拔掉走蓝牙（原来的行为）
 *   长按 RG 2 秒 -> 切到「强制蓝牙」：即使 USB 插着也走蓝牙，USB HID 报告暂停
 *   再长按 2 秒 -> 切回「自动」：暂停蓝牙输入并保活链路，交还 USB
 *
 * 切到强制蓝牙时会先向主机发送一帧空报告把按键松开，
 * 避免主机看到"某个键一直被按住"。
 * ========================================================================== */

#define MODE_SWITCH_HOLD_MS   2000U     /* 长按 2 秒触发 */
#define MODE_SCAN_STEP_MS     10U       /* 与 UsbHid_Task 的扫描节拍一致 */

/* ---- 模式切换的视觉反馈：600ms 内闪 3 次 ---- */
#define MODE_FLASH_TOTAL_MS   600U
#define MODE_FLASH_STEP_MS    100U

/* ---- 模式掉电记忆（独立 Flash 页，与蓝牙绑定页 0x0803F800 分开） ---- */
#define MODE_FLASH_ADDR       0x0803F000U
#define MODE_MAGIC            0x4D4F4445U   /* "MODE" */

static uint8_t  s_btPref   = 0U;        /* 用户偏好：0 = 自动(USB 优先)，1 = 强制蓝牙（掉电保存） */
static uint8_t  s_usbWasIn = 0xFFU;     /* 上次的 USB 挂载状态（0xFF = 还没记录） */
static uint8_t  s_btEngaged = 0U;       /* 用户是否主动要求走蓝牙（长按置 1；回落/插线清 0） */
static uint8_t  s_btEff    = 0U;        /* 实际输出：1 = 当前走蓝牙（每 10ms 实时计算） */
static uint32_t s_rgPressStartMs = 0U;  /* RG 按下的时刻（0 = 当前没按） */
static uint8_t  s_rgTriggered    = 0U;  /* 本次长按是否已经触发过 */

/* 最近一次实际输出的 LED 颜色（应用下发或彩虹）；闪烁不更新它，便于恢复 */
static uint8_t  s_ledR = 0U;
static uint8_t  s_ledG = 0U;
static uint8_t  s_ledB = 0U;

static uint8_t  s_flashActive  = 0U;
static uint32_t s_flashStartMs = 0U;

/* 上电静默期：启动过程会经历"蓝牙->USB回落->蓝牙"的状态迁移，
 * 这段时间不因为"自动切换"闪灯，避免一上电就蓝白乱闪。 */
#define MODE_FLASH_GRACE_MS   5000U
static uint8_t s_flashGrace = 1U;

/* 统一的 LED 输出入口：记录当前颜色再写 PWM */
static void led_apply(uint8_t r, uint8_t g, uint8_t b)
{
    s_ledR = r;
    s_ledG = g;
    s_ledB = b;
    Tim2_SetRedBrightness(r);
    Tim2_SetGreenBrightness(g);
    Tim2_SetBlueBrightness(b);
}

/* 切回 USB 模式时通知蓝牙侧断开链路（实现在 bt_gamepad.c） */
extern void BT_AppForceUsbMode(void);
/* 蓝牙 HID 通道是否就绪（实现在 bt_gamepad.c），用于组装状态字节 */
extern uint8_t BT_AppIsLinked(void);
/* 诊断用：回连次数 / 蓝牙状态机状态 / 最近断开原因码 */
extern uint8_t  BT_AppReconnectTries(void);
extern uint8_t  BT_AppState(void);
extern volatile uint8_t g_bt_last_disc_reason;
extern uint8_t bt_hid_chan_state(void);     /* 诊断：HID 通道状态 */
extern void BT_AppForceBtMode(void);        /* 切到蓝牙时彻底复位链路并回连 */

/* 自定义报告 rep[3] = 状态字节（上次已经上报过的值）：
 *   bit0 = 1  当前是「强制蓝牙」模式
 *   bit1 = 1  蓝牙 HID 通道已就绪
 * 上位机据此显示当前工作模式。 */
static uint8_t s_status_reported = 0xFFU;
static uint8_t s_diag_reason = 0xFFU;   /* 诊断字段上次上报值 */
static uint8_t s_diag_tries  = 0xFFU;
static uint8_t s_diag_state  = 0xFFU;
static uint8_t s_diag_chan   = 0xFFU;

/* 计算当前状态字节 */
static uint8_t custom_status_byte(void)
{
    uint8_t st = 0U;

    if (s_btEff != 0U)              /* bit0：当前键鼠输出走蓝牙 */
    {
        st |= 0x01U;
    }
    if (BT_AppIsLinked() != 0U)     /* bit1：蓝牙 HID 链路已就绪 */
    {
        st |= 0x02U;
    }
    if (s_btPref != 0U)             /* bit2：用户设了"强制蓝牙"偏好 */
    {
        st |= 0x04U;
    }
    return st;
}

uint8_t UsbHid_IsBtForced(void)
{
    return s_btEff;                 /* 实际是否走蓝牙（USB HID 报告暂停的判断） */
}

uint8_t UsbHid_IsBtPreferred(void)
{
    return s_btPref;                /* 用户偏好：是否允许蓝牙（决定蓝牙任务是否继续跑） */
}

/* 把当前模式写进 Flash（页 126） */
static void mode_flash_save(uint8_t forceBt)
{
    uint32_t words[2];
    uint8_t  i;

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    (void)FLASH_ErasePage(MODE_FLASH_ADDR);

    words[0] = MODE_MAGIC;
    words[1] = (forceBt != 0U) ? 1U : 0U;
    for (i = 0U; i < 2U; i++)
    {
        (void)FLASH_ProgramWord(MODE_FLASH_ADDR + ((uint32_t)i * 4U), words[i]);
    }
    FLASH_Lock();
}

/* 从 Flash 读回上次的模式；没有记录（首次上电）按自动处理 */
static uint8_t mode_flash_load(void)
{
    const uint32_t *p = (const uint32_t *)MODE_FLASH_ADDR;

    if (p[0] != MODE_MAGIC)
    {
        return 0U;
    }
    return (p[1] != 0U) ? 1U : 0U;
}

/* 闪灯结束后把 CapsLock/NumLock 指示灯状态重新打回去（定义在后面，需前置声明） */
static void led_reapply_lock_indicators(void);

/* 启动视觉反馈：三闪 */
static void mode_flash_start(void)
{
    s_flashActive  = 1U;
    s_flashStartMs = s_millis;
}

/* 闪烁任务：600ms 内按 100ms 交替亮灭，结束后恢复原色 */
static void mode_flash_task(void)
{
    uint32_t elapsed;
    uint8_t  on;

    if (s_flashActive == 0U)
    {
        return;
    }

    elapsed = (uint32_t)(s_millis - s_flashStartMs);
    if (elapsed >= MODE_FLASH_TOTAL_MS)
    {
        s_flashActive = 0U;
        led_apply(s_ledR, s_ledG, s_ledB);      /* 恢复应用/彩虹原本设的颜色 */
        led_reapply_lock_indicators();          /* 再把 CapsLock/NumLock 指示灯打回去 */
        return;
    }

    on = (((elapsed / MODE_FLASH_STEP_MS) & 1U) == 0U) ? 100U : 0U;

    /* 直接写 PWM，不经过 led_apply —— 闪烁不是"当前颜色"。
     * 颜色跟随"实际输出"：走蓝牙闪蓝、走 USB 闪白。 */
    if (s_btEff != 0U)
    {
        Tim2_SetRedBrightness(0U);  Tim2_SetGreenBrightness(0U);  Tim2_SetBlueBrightness(on);
    }
    else
    {
        Tim2_SetRedBrightness(on);  Tim2_SetGreenBrightness(on);  Tim2_SetBlueBrightness(on);
    }
}

/* 插线优先 USB：检测"刚插上数据线"这个事件，把模式强制切回"自动"。
 * 用事件而不是持续判断 —— 这样插着线时用户长按 RG 切到蓝牙仍然有效，
 * 直到下次重新插线才会再回到 USB。 */
static void mode_usb_plug_check(void)
{
    uint8_t mounted = (uint8_t)(tud_mounted() ? 1u : 0u);

    if ((mounted != 0u) && (s_usbWasIn == 0u))
    {
        /* 刚插上线：一律走 USB */
        s_btEngaged = 0u;           /* 插线一律回到 USB 优先 */
        if (s_btPref != 0u)
        {
            s_btPref = 0u;
            mode_flash_save(0u);
            UART1_SendString("[mode] USB plugged -> AUTO (USB priority)\r\n");
        }
    }
    s_usbWasIn = mounted;
}

/* 实时计算"实际输出走哪条通道"，由 UsbHid_Task 每 10ms 调一次。
 *
 *   偏好=自动                    -> 走 USB（有 USB 时）；没插 USB 才用蓝牙
 *   偏好=强制蓝牙 + 蓝牙已连接   -> 走蓝牙
 *   偏好=强制蓝牙 + 蓝牙未连 + 插着 USB -> 暂走 USB（不会出现"没输入"的死区）
 *   偏好=强制蓝牙 + 蓝牙未连 + 没插 USB -> 走蓝牙
 *
 * 这样电脑关掉蓝牙后会立刻回落 USB；蓝牙重新连上后自动切回蓝牙，
 * 不需要用户再长按一次。 */
static void mode_effective_update(void)
{
    uint8_t eff = 0U;
    uint8_t mounted = (uint8_t)(tud_mounted() ? 1u : 0u);

    if (mounted == 0u)
    {
        eff = 1U;                   /* 没插数据线：蓝牙是唯一选择 */
    }
    else if ((s_btPref != 0U) && (s_btEngaged != 0U) && (BT_AppIsLinked() != 0U))
    {
        eff = 1U;                   /* 插着线 + 用户主动要求蓝牙 + 确实连上了 */
    }
    /* 其余情况（插着线时）一律走 USB。
     * 注意这里【不做超时放弃】：蓝牙回连可能要试 3~4 次、20 秒左右，
     * 超时放弃会把"长按切蓝牙"掐死在半路（第一次切就卡住）。 */

    /* 从蓝牙掉回 USB（蓝牙中途断了）→ 清掉"主动要求"，
     * 这样电脑蓝牙再恢复也不会自己抢回输出，必须用户再长按。 */
    if ((eff == 0U) && (s_btEff != 0U) && (mounted != 0u))
    {
        s_btEngaged = 0U;
    }

    if (eff != s_btEff)
    {
        s_btEff = eff;

        if (s_flashGrace == 0U)
        {
            mode_flash_start();         /* 自动切换也给视觉反馈（上电初期不闪） */
        }

        Joystick_RecalibrateNow();      /* 蓝牙射频状态变了，重标摇杆中位 */
        UART1_Printf("[mode] effective -> %s\r\n",
                     (eff != 0U) ? "BLUETOOTH" : "USB");
    }
}

/* 模式切换检测，由 UsbHid_Task 每 10ms 调一次（未挂载时也要检测） */
static void mode_switch_scan(void)
{
    /* Key_IsDown 返回 1 = 消抖后按下，0 = 松开。
     * 用时间戳判定长按（不用累加计数）：任务被 Flash 擦写等操作拖慢时
     * 累加计数会算少，导致"有时候长按不生效"。 */
    if (Key_IsDown(KEY_RG) == 0u)
    {
        s_rgPressStartMs = 0U;
        s_rgTriggered    = 0U;
        return;
    }

    if (s_rgTriggered != 0U)
    {
        return;                         /* 已触发，等松手后再重新武装 */
    }

    if (s_rgPressStartMs == 0U)
    {
        s_rgPressStartMs = s_millis;    /* 记录按下时刻 */
        return;
    }

    if ((uint32_t)(s_millis - s_rgPressStartMs) < MODE_SWITCH_HOLD_MS)
    {
        return;
    }

    s_rgTriggered = 1U;

    /* 根据当前实际输出切换，而不是直接翻转 s_btEngaged。
     * 独立供电转接 USB、蓝牙断线回落等过程中，s_btEngaged 可能已被
     * 插线回调或回落逻辑清掉；若仍对它取反，用户想切回 USB 时反而会
     * 再次进入蓝牙。当前走蓝牙就明确切 AUTO，否则明确进入蓝牙。 */
    s_btEngaged = (s_btEff != 0U) ? 0U : 1U;
    s_btPref = s_btEngaged;

    if (s_btPref != 0U)
    {
        /* 进入强制蓝牙：先把 USB 上的按键全部松开，再停发 USB 报告 */
        uint8_t empty[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

        (void)tud_hid_n_keyboard_report(USB_HID_INST_KEYBOARD, 0U, 0U, empty);
        (void)tud_hid_n_mouse_report(USB_HID_INST_MOUSE, 0U, 0U, 0, 0, 0, 0);
        UART1_SendString("[mode] FORCE BLUETOOTH (USB HID paused)\r\n");

        /* 切到蓝牙时如果还挂着链路，重建一次拿干净通道 */
        BT_AppForceBtMode();
    }
    else
    {
        UART1_SendString("[mode] AUTO / USB priority\r\n");
        BT_AppForceUsbMode();           /* 断开蓝牙链路，把输入交还 USB */
    }

    mode_flash_start();                 /* 视觉反馈：切蓝牙闪蓝、切回闪白 */
    mode_flash_save(s_btPref);          /* 掉电记忆（记的是用户偏好） */

    /* 切换后立刻重标摇杆中位：蓝牙射频工作会让摇杆静止点偏移，
     * 不重标就会出现"切到蓝牙后光标又自己往上飘"。 */
    Joystick_RecalibrateNow();
}

void UsbHid_Init(void)
{
    GPIO_InitTypeDef gpio;
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };

    /* 第 1 步：USB 时钟 = 72MHz / 1.5 = 48MHz。
     * RM0008 要求 USBPRE 必须在 USBEN 之前配置好，顺序不能换。 */
    RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

    /* 第 2 步：PA11 = USB_DM，PA12 = USB_DP，复用推挽。
     * 这一步会覆盖 PreInit 里"D+ 输出低"的设置，主机这时才看到插入。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_11 | GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    UsbHid_TimeBaseInit();
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 2);
    NVIC_SetPriority(USB_HP_CAN1_TX_IRQn, 2);

    /* 第 4 步：初始化板载按键与两个摇杆按键的独立状态机 */
    Key_FSM_Init();
    Joystick_KeyInit();

    /* 第 5 步：从 Flash 读回上次保存的工作模式（首次上电没有记录按"自动"处理） */
    s_btPref = mode_flash_load();
    s_btEngaged = s_btPref;         /* 恢复偏好时也必须恢复主动选路状态 */
    if (s_btPref != 0U)
    {
        UART1_SendString("[mode] restored: FORCE BLUETOOTH (from flash)\r\n");
    }

    /* 第 6 步：启动 TinyUSB 设备协议栈 */
    tusb_init(0, &dev_init);
}

/* USB 中断入口。TinyUSB 的 dcd 层只要求把中断转进来，
 * 具体是哪个端点、该读该写都由协议栈自己判断。 */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    dcd_int_handler(0);
}

void USB_HP_CAN1_TX_IRQHandler(void)
{
    dcd_int_handler(0);
}

/* 键盘 LED 状态由当前主机链路共同维护，自定义 HID 用它回传 Caps / Num。 */
static uint8_t s_led_state = 0U;

void UsbHid_SetKeyboardLeds(uint8_t leds)
{
    leds &= 0x03U;
    if (s_led_state == leds)
    {
        return;
    }

    s_led_state = leds;
    Tim2_SetGreenBrightness((leds & 0x01U) != 0U ? 60U : 0U);
    Tim2_SetRedBrightness((leds & 0x02U) != 0U ? 60U : 0U);
    UART1_Printf("[led] num=%u caps=%u\r\n",
                 (unsigned)(leds & 0x01U),
                 (unsigned)((leds >> 1) & 0x01U));
}

uint8_t UsbHid_GetKeyboardLeds(void)
{
    return s_led_state;
}

/* 闪灯结束后把 CapsLock/NumLock 指示灯状态重新打回去。
 * 原因：模式切换的三闪会临时改写 RGB，恢复时用的是"应用/彩虹颜色"，
 * 会把锁定指示灯的状态覆盖掉 —— 表现为"切换模式后锁定灯灭了"。
 * UsbHid_SetKeyboardLeds() 有"值没变就跳过"的优化，
 * 这里先把缓存改成不可能值，强制它重新输出一遍。 */
static void led_reapply_lock_indicators(void)
{
    uint8_t leds = (uint8_t)(s_led_state & 0x03U);

    s_led_state = 0xFFU;
    UsbHid_SetKeyboardLeds(leds);
}

/* 自定义 HID 上行状态 */
static uint8_t  s_key_bitmap   = 0U;
static uint8_t  s_stick_bitmap = 0U;  /* bit0=L3/左 Shift，bit1=R3/左 Ctrl */
static uint8_t  s_led_reported = 0xFFU;  /* 上次已上报的锁定状态，初值取不可能值，强制上电先发一帧 */
static uint8_t  s_seq          = 0U;
static uint32_t s_last_beat_ms = 0U;

static uint8_t  s_kbd_hold       = 0U;
static uint8_t  s_kbd_modifier   = 0U;  /* 独立修饰键，可与普通键码同时上报 */
static uint8_t  s_kbd_raw        = 0U;
static uint8_t  s_kbd_stable     = 0U;
static uint8_t  s_kbd_dirty      = 1U;  /* 端点忙时保留待发状态，不能直接丢掉 */

/* Caps / Num 各自独立消抖和点按计时，互不占用手动事件槽。 */
#define USB_LOCK_KEY_COUNT        2U
static const uint8_t s_lock_key_id[USB_LOCK_KEY_COUNT]   = { KEY_X, KEY_Y };
static const uint8_t s_lock_hid_code[USB_LOCK_KEY_COUNT] = { 0x39U, 0x53U };
static uint8_t  s_lock_raw[USB_LOCK_KEY_COUNT];
static uint8_t  s_lock_down[USB_LOCK_KEY_COUNT];
static uint8_t  s_lock_debounce[USB_LOCK_KEY_COUNT];
static uint8_t  s_lock_tap_active[USB_LOCK_KEY_COUNT];
static uint8_t  s_lock_tap_sent[USB_LOCK_KEY_COUNT];
static uint32_t s_lock_tap_ms[USB_LOCK_KEY_COUNT];

/* 鼠标上次的按键状态，用来判断"有没有变化、要不要发报告" */
static uint8_t  s_mouse_buttons  = 0U;
static uint32_t s_wheel_ms       = 0U;

/* 扫描节拍 */
static uint32_t s_last_scan_ms = 0U;
#define USB_SCAN_INTERVAL_MS     10U
#define USB_WHEEL_INTERVAL_MS    100U

#define USB_JOY_AXIS_ROTATE      1

/* 摇杆释放后仍可能有约 10% 的零点偏移。死区在坐标旋转后生效，
 * 同时覆盖光标 X/Y；取约 18% 满量程，避免静止偏移持续产生位移。 */
#define USB_JOY_DEADZONE         9000   /* 与蓝牙路径一致：静止偏移小于 27% 都按 0 处理 */
#define USB_JOY_MOUSE_MAX        12
#define USB_WHEEL_THRESHOLD      12000
#define USB_MOUSE_FILTER_SAMPLES 8U

/* 极性微调：实测某个方向反了，把对应宏改成 1 即可 */
#define USB_MOUSE_INVERT_X       0
#define USB_MOUSE_INVERT_Y       0

/* 滚轮方向：HID wheel 为正表示向上滚（远离用户）。
 * 用旋转后的"逻辑上"分量判断，方向反了把本宏改成 1。 */
#define USB_WHEEL_INVERT         0

/* 光标轴先对原始 ADC 做平均，再相对校准中点映射。
 * 这样滤波不会抹掉中点校准结果，也能抑制单帧 ADC 毛刺。 */
static int32_t mouse_read_axis(uint8_t axis)
{
    uint32_t sum = 0U;
    uint8_t i;

    for (i = 0U; i < USB_MOUSE_FILTER_SAMPLES; i++)
    {
        sum += Joystick_ReadAxis(axis);
    }

    return Joystick_MapRawValue(axis,
                                (uint16_t)((sum + (USB_MOUSE_FILTER_SAMPLES / 2U))
                                           / USB_MOUSE_FILTER_SAMPLES));
}

/* 把 ADC 原始轴旋转成逻辑坐标：mx = 右为正，my = 上为正。
 * lx/ly 是 Joystick_MapRawValue() 的结果，范围 -32767~32767。 */
static void joy_rotate(int32_t lx, int32_t ly, int32_t *mx, int32_t *my)
{
    switch (USB_JOY_AXIS_ROTATE)
    {
        case 1:  *mx =  ly; *my = -lx; break;
        case 2:  *mx = -lx; *my = -ly; break;
        case 3:  *mx = -ly; *my =  lx; break;
        default: *mx =  lx; *my =  ly; break;
    }
}

static int8_t axis_to_move(int32_t v)
{
    int32_t a = (v < 0) ? -v : v;
    int32_t t;
    int32_t n;
    int32_t m;

    if (a <= USB_JOY_DEADZONE)
    {
        return 0;
    }

    t = a - USB_JOY_DEADZONE;
    n = 32767 - USB_JOY_DEADZONE;

    m = (int32_t)(((int64_t)t * t * USB_JOY_MOUSE_MAX + (int64_t)n * n / 2)
                  / ((int64_t)n * n));
    if (m < 1)
    {
        m = 1;
    }
    else if (m > USB_JOY_MOUSE_MAX)
    {
        m = USB_JOY_MOUSE_MAX;
    }

    return (v < 0) ? (int8_t)(-m) : (int8_t)m;
}

/* 点按（CapsLock / NumLock）成功发出按下报告后，至少保持 30ms 再释放。 */
#define USB_KBD_TAP_MS           30U

static void kbd_reset_state(void)
{
    s_kbd_hold     = 0U;
    s_kbd_modifier = 0U;
    s_kbd_raw    = 0U;
    s_kbd_stable = 0U;
    s_kbd_dirty  = 1U;

    memset(s_lock_raw, 0, sizeof(s_lock_raw));
    memset(s_lock_down, 0, sizeof(s_lock_down));
    memset(s_lock_debounce, 0, sizeof(s_lock_debounce));
    memset(s_lock_tap_active, 0, sizeof(s_lock_tap_active));
    memset(s_lock_tap_sent, 0, sizeof(s_lock_tap_sent));
    memset(s_lock_tap_ms, 0, sizeof(s_lock_tap_ms));
}

static void kbd_flush(void)
{
    uint8_t keys[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
    uint8_t n = 0U;
    uint8_t i;

    if (s_kbd_hold != 0U)
    {
        keys[n++] = s_kbd_hold;
    }
    for (i = 0U; i < USB_LOCK_KEY_COUNT; i++)
    {
        if (s_lock_tap_active[i] != 0U)
        {
            keys[n++] = s_lock_hid_code[i];
        }
    }

    if (!tud_hid_n_ready(USB_HID_INST_KEYBOARD))
    {
        s_kbd_dirty = 1U;
        return;
    }

    if (tud_hid_n_keyboard_report(USB_HID_INST_KEYBOARD, 0U, s_kbd_modifier, keys))
    {
        s_kbd_dirty = 0U;
        for (i = 0U; i < USB_LOCK_KEY_COUNT; i++)
        {
            if ((s_lock_tap_active[i] != 0U) && (s_lock_tap_sent[i] == 0U))
            {
                /* 只有按下报告真正交给 USB 后才开始计时，避免端点忙把点按吞掉。 */
                s_lock_tap_sent[i] = 1U;
                s_lock_tap_ms[i] = s_millis;
            }
        }
    }
    else
    {
        s_kbd_dirty = 1U;
    }
}

static void kbd_scan(void)
{
    uint8_t want = 0U;
    uint8_t modifier = 0U;
    uint8_t i;

    /* L3/R3 已由各自的独立状态机完成 20ms 消抖；这里只组合 HID 修饰键位。
     * 两键可叠加，且不影响 UP/DN/X/Y 的普通键码。 */
    if (Joystick_KeyIsDown(JOYSTICK_L3) != 0U)
    {
        modifier |= 0x02U;          /* HID Modifier: Left Shift */
    }
    if (Joystick_KeyIsDown(JOYSTICK_R3) != 0U)
    {
        modifier |= 0x01U;          /* HID Modifier: Left Ctrl */
    }
    if (modifier != s_kbd_modifier)
    {
        s_kbd_modifier = modifier;
        s_kbd_dirty = 1U;
        UART1_Printf("[usb] modifier 0x%02X (L3=Shift, R3=Ctrl)\r\n",
                     (unsigned)modifier);
    }

    /* --- UP / DN：先采样，再 20ms 消抖 --- */
    if (Key_IsDown(KEY_UP) != 0u)
    {
        want = 0x2CU;               /* HID_KEY_SPACE：主键盘空格 */
    }
    else if (Key_IsDown(KEY_DN) != 0u)
    {
        want = 0x28U;               /* HID_KEY_ENTER：主键盘回车 */
    }

    if (want != s_kbd_raw)
    {
        s_kbd_raw    = want;
        s_kbd_stable = 0U;
    }
    else if (s_kbd_stable < 2U)
    {
        s_kbd_stable++;
    }

    if ((s_kbd_stable >= 2U) && (s_kbd_hold != s_kbd_raw))
    {
        s_kbd_hold = s_kbd_raw;
        s_kbd_dirty = 1U;
        UART1_Printf("[usb] kbd hold 0x%02X\r\n", (unsigned)s_kbd_hold);
    }

    /* --- X / Y：独立消抖，确认按下时触发一次，长按不会变成“无事件”。 --- */
    for (i = 0U; i < USB_LOCK_KEY_COUNT; i++)
    {
        if ((s_lock_tap_active[i] != 0U) && (s_lock_tap_sent[i] != 0U) &&
            ((uint32_t)(s_millis - s_lock_tap_ms[i]) >= USB_KBD_TAP_MS))
        {
            s_lock_tap_active[i] = 0U;
            s_lock_tap_sent[i] = 0U;
            s_kbd_dirty = 1U;
        }

        want = (Key_IsDown(s_lock_key_id[i]) != 0u) ? 1U : 0U;
        if (want != s_lock_raw[i])
        {
            s_lock_raw[i] = want;
            s_lock_debounce[i] = 0U;
        }
        else if (s_lock_debounce[i] < 2U)
        {
            s_lock_debounce[i]++;
        }

        if ((s_lock_debounce[i] >= 2U) && (s_lock_down[i] != want))
        {
            s_lock_down[i] = want;
            if (want != 0U)
            {
                s_lock_tap_active[i] = 1U;
                s_lock_tap_sent[i] = 0U;
                s_lock_tap_ms[i] = s_millis;
                s_kbd_dirty = 1U;
            }
        }
    }

    /* 每次扫描都重试未成功发送的键盘状态；上位机打开自定义 HID 后
     * 端点调度更忙，也不能因此丢掉整个键盘接口的按下或释放报告。 */
    if (s_kbd_dirty != 0U)
    {
        kbd_flush();
    }
}

/* 10ms 一次的鼠标扫描 */
static void mouse_scan(void)
{
    int32_t  lx, ly, mx, my;
    int32_t  rx, ry, wx, wy;
    int8_t   dx = 0;
    int8_t   dy = 0;
    int8_t   wheel = 0;
    uint8_t  buttons = 0U;

    /* 按键：统一用状态机消抖后的稳定状态 Key_IsDown()（1 = 按下） */
    if (Key_IsDown(KEY_A) != 0u)
    {
        buttons |= 0x01U;       /* 左键 */
    }
    if (Key_IsDown(KEY_B) != 0u)
    {
        buttons |= 0x02U;       /* 右键 */
    }
    if (Key_IsDown(KEY_LF) != 0u)
    {
        buttons |= 0x04U;       /* 中键 */
    }

    /* 左摇杆 -> 光标位移：原始 ADC 滤波，再旋转、死区和速度映射 */
    lx = mouse_read_axis(JOYSTICK_LX);
    ly = mouse_read_axis(JOYSTICK_LY);
    joy_rotate(lx, ly, &mx, &my);

    dx = axis_to_move(mx);
    dy = (int8_t)(-axis_to_move(my));  /* HID dy 正 = 向下，而 my 正 = 向上 */

#if USB_MOUSE_INVERT_X
    dx = (int8_t)(-dx);
#endif
#if USB_MOUSE_INVERT_Y
    dy = (int8_t)(-dy);
#endif

    if ((uint32_t)(s_millis - s_wheel_ms) >= USB_WHEEL_INTERVAL_MS)
    {
        s_wheel_ms = s_millis;

        /* 右摇杆同样旋转 90 度安装，取旋转后的"逻辑上"分量 wy */
        rx = Joystick_MapRawValue(JOYSTICK_RX, Joystick_ReadAxis(JOYSTICK_RX));
        ry = Joystick_MapRawValue(JOYSTICK_RY, Joystick_ReadAxis(JOYSTICK_RY));
        joy_rotate(rx, ry, &wx, &wy);
        (void)wx;

        if (wy > USB_WHEEL_THRESHOLD)
        {
            wheel = 1;
        }
        else if (wy < -USB_WHEEL_THRESHOLD)
        {
            wheel = -1;
        }

#if USB_WHEEL_INVERT
        wheel = (int8_t)(-wheel);
#endif
    }

    /* 没有位移、没有滚轮、按键也没变化 -> 不发报告，省带宽 */
    if ((dx == 0) && (dy == 0) && (wheel == 0) && (buttons == s_mouse_buttons))
    {
        return;
    }

    if (!tud_hid_n_ready(USB_HID_INST_MOUSE))
    {
        return;
    }

    if (tud_hid_n_mouse_report(USB_HID_INST_MOUSE, 0U, buttons, dx, dy, wheel, 0))
    {
        s_mouse_buttons = buttons;
    }
}

/* 彩虹渐变由板端执行：上位机只发开始 / 停止命令，避免持续 USB 灯控传输。 */
#define USB_RAINBOW_INTERVAL_MS  40U
static uint8_t  s_rainbow_enabled = 0U;
static uint16_t s_rainbow_hue = 0U;
static uint32_t s_rainbow_last_ms = 0U;

void UsbHid_SetRainbowMode(uint8_t on)
{
    s_rainbow_enabled = (on != 0U) ? 1U : 0U;
    if (s_rainbow_enabled != 0U)
    {
        s_rainbow_last_ms = s_millis - USB_RAINBOW_INTERVAL_MS;
    }
}

static void rainbow_task(void)
{
    uint8_t region;
    uint8_t rise;
    uint8_t fall;

    if ((s_rainbow_enabled == 0U) || (s_flashActive != 0U) ||
        ((uint32_t)(s_millis - s_rainbow_last_ms) < USB_RAINBOW_INTERVAL_MS))
    {
        return;                 /* 闪烁反馈期间不更新彩虹，避免互相覆盖 */
    }
    s_rainbow_last_ms = s_millis;

    region = (uint8_t)(s_rainbow_hue / 60U);
    rise = (uint8_t)(((uint32_t)(s_rainbow_hue % 60U) * 100U) / 60U);
    fall = (uint8_t)(100U - rise);

    switch (region)
    {
        case 0U: led_apply(100U, rise, 0U);   break;
        case 1U: led_apply(fall, 100U, 0U);   break;
        case 2U: led_apply(0U, 100U, rise);   break;
        case 3U: led_apply(0U, fall, 100U);   break;
        case 4U: led_apply(rise, 0U, 100U);   break;
        default: led_apply(100U, 0U, fall);   break;
    }

    s_rainbow_hue = (uint16_t)((s_rainbow_hue + 4U) % 360U);
}

static void custom_scan(void)
{
    uint8_t  bm = 0U;
    uint8_t  joy = 0U;
    uint8_t  led;
    uint8_t  rep[USB_HID_CUSTOM_REPORT_LEN];
    uint8_t  k;
    uint8_t  changed;
    uint8_t  st;

    for (k = KEY_A; k <= KEY_RG; k++)
    {
        if (Key_IsDown(k) != 0u)
        {
            bm |= (uint8_t)(1U << (k - KEY_A));
        }
    }

    if (Joystick_KeyIsDown(JOYSTICK_L3) != 0U) { joy |= 0x01U; }
    if (Joystick_KeyIsDown(JOYSTICK_R3) != 0U) { joy |= 0x02U; }

    /* 第 3 字节回传锁定状态：bit0 NumLock、bit1 CapsLock。 */
    led = UsbHid_GetKeyboardLeds();

    st = custom_status_byte();

    /* 诊断字段也纳入变化检测：断开/回连时上位机能马上看到 */
    changed = ((bm != s_key_bitmap) || (joy != s_stick_bitmap) ||
               (led != s_led_reported) || (st != s_status_reported) ||
               (g_bt_last_disc_reason != s_diag_reason) ||
               (BT_AppReconnectTries() != s_diag_tries) ||
               (BT_AppState() != s_diag_state) ||
               (bt_hid_chan_state() != s_diag_chan)) ? 1U : 0U;

    if ((changed == 0U) &&
        ((uint32_t)(s_millis - s_last_beat_ms) < 1000U))
    {
        return;
    }

    if (!tud_hid_n_ready(USB_HID_INST_CUSTOM))
    {
        return;
    }

    memset(rep, 0, sizeof(rep));
    rep[0] = bm;
    rep[1] = s_seq;
    rep[2] = led;
    rep[3] = st;                    /* 工作模式 + 蓝牙链路状态，供上位机显示 */
    rep[4] = g_bt_last_disc_reason; /* 诊断：最近断开原因码（0xFF=没断过） */
    rep[5] = BT_AppReconnectTries();/* 诊断：回连尝试次数 */
    rep[6] = BT_AppState();         /* 诊断：蓝牙状态机状态（0~3） */
    rep[7] = bt_hid_chan_state();   /* 诊断：HID 通道状态（bit0控制 bit1中断 bit2可发） */
    rep[8] = joy;                   /* 新增：bit0=L3/Shift，bit1=R3/Ctrl */

    if (tud_hid_n_report(USB_HID_INST_CUSTOM, 0U, rep, (uint16_t)sizeof(rep)))
    {
        s_key_bitmap   = bm;
        s_stick_bitmap = joy;
        s_led_reported = led;
        s_status_reported = st;
        s_diag_reason = g_bt_last_disc_reason;
        s_diag_tries  = BT_AppReconnectTries();
        s_diag_state  = BT_AppState();
        s_diag_chan   = bt_hid_chan_state();
        s_seq++;
        s_last_beat_ms = s_millis;
    }
}

void UsbHid_Task(void)
{
    uint32_t now;

    tud_task();                     /* 未挂载时也必须持续推进协议栈 */
    now = s_millis;
    if ((uint32_t)(now - s_last_scan_ms) < USB_SCAN_INTERVAL_MS)
    {
        return;
    }
    s_last_scan_ms = now;
    Key_FSM_Update();
    Joystick_KeyUpdate();           /* 全工程唯一更新点，USB/BT 共用消抖结果 */

    /* 上电 5 秒后解除"不闪灯"限制 */
    if ((s_flashGrace != 0U) && (s_millis >= MODE_FLASH_GRACE_MS))
    {
        s_flashGrace = 0U;
    }

    mode_usb_plug_check();      /* 插上数据线 -> 强制回到 USB 优先 */
    mode_switch_scan();         /* 长按 RG 切 USB / 蓝牙（未挂载也要检测） */
    mode_effective_update();    /* 实时决定当前走 USB 还是蓝牙（蓝牙断开自动回落） */
    mode_flash_task();          /* 模式切换的三闪反馈（结束才恢复原色） */
    rainbow_task();             /* USB 和蓝牙模式都执行板端彩虹 */

    if (!tud_mounted())
    {
        /* 蓝牙期间不保留 USB 键盘状态，重新挂载后从当前电平重新消抖。 */
        kbd_reset_state();
        s_mouse_buttons = 0U;
        return;
    }

    if (s_btEff != 0U)
    {
        /* 当前输出走蓝牙：键盘/鼠标报告停发（否则会和蓝牙争输入），
         * 但【自定义 HID 仍然发送】—— 上位机靠它显示当前工作模式和按键状态。 */
        kbd_reset_state();
        s_mouse_buttons = 0U;
        custom_scan();
        return;
    }

    kbd_scan();
    mouse_scan();
    custom_scan();
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)report_id;
    (void)report_type;

    if (instance == USB_HID_INST_KEYBOARD)
    {
        if (reqlen < 8U)
        {
            return 0U;
        }

        memset(buffer, 0, 8U);
        return 8U;
    }

    if (instance == USB_HID_INST_CUSTOM)
    {
        if (reqlen < USB_HID_CUSTOM_REPORT_LEN)
        {
            return 0U;
        }

        memset(buffer, 0, USB_HID_CUSTOM_REPORT_LEN);
        buffer[0] = s_key_bitmap;
        buffer[1] = s_seq;
        buffer[2] = UsbHid_GetKeyboardLeds();
        buffer[3] = custom_status_byte();
        buffer[4] = g_bt_last_disc_reason;
        buffer[5] = BT_AppReconnectTries();
        buffer[6] = BT_AppState();
        buffer[7] = bt_hid_chan_state();
        buffer[8] = s_stick_bitmap;
        return USB_HID_CUSTOM_REPORT_LEN;
    }

    return 0U;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;

    if (bufsize == 0U)
    {
        return;
    }

    if ((instance == USB_HID_INST_KEYBOARD) &&
        (report_type == HID_REPORT_TYPE_OUTPUT))
    {
        UsbHid_SetKeyboardLeds(buffer[0]);
        return;
    }

    /* 回调内只记录命令或更新 PWM 占空比，禁止串口打印。 */
    if ((instance == USB_HID_INST_CUSTOM) &&
        (report_type == HID_REPORT_TYPE_OUTPUT))
    {
        switch (buffer[0])
        {
            case USB_HID_CMD_SET_RGB:
                if (bufsize >= 4U)
                {
                    s_rainbow_enabled = 0U;
                    led_apply(buffer[1], buffer[2], buffer[3]);
                }
                break;

            case USB_HID_CMD_RAINBOW_START:
                s_rainbow_enabled = 1U;
                s_rainbow_last_ms = s_millis - USB_RAINBOW_INTERVAL_MS;
                break;

            case USB_HID_CMD_RAINBOW_STOP:
                s_rainbow_enabled = 0U;
                break;

            default:
                break;
        }
    }
}

/* 枚举完成 / 拔出：打印一行，方便串口里确认主机到底认没认这个设备 */
void tud_mount_cb(void)
{
    kbd_reset_state();
    s_key_bitmap = 0U;
    s_led_reported = 0xFFU;

    /* USB 枚举完成是明确的“插入数据线”事件，必须立即回到 USB。
     * 不能只等 10ms 扫描里的边沿检测，否则时基/扫描状态异常时会出现
     * 电脑已经识别 USB，但固件仍停留在强制蓝牙的情况。 */
    s_usbWasIn = 1U;
    s_btEngaged = 0U;
    s_btEff = 0U;
    if (s_btPref != 0U)
    {
        s_btPref = 0U;
        mode_flash_save(0U);
    }
    /* 独立供电时蓝牙通常仍处于连接状态；重新接上电脑并完成枚举后，
     * 立即松开蓝牙按键并交还 USB 输入，但保留蓝牙链路用于快速切回。 */
    BT_AppForceUsbMode();
    mode_flash_start();             /* 白色三闪：输入已经交还 USB */
    UART1_SendString("[usb] mounted; AUTO / USB priority, input -> USB\r\n");
}

void tud_umount_cb(void)
{
    s_mouse_buttons = 0U;
    kbd_reset_state();
    s_key_bitmap = 0U;
    s_led_reported = 0xFFU;
    s_usbWasIn = 0U;                /* 为下一次插入重新武装挂载边沿 */
    UART1_SendString("[usb] unmounted; Bluetooth may take over\r\n");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
}

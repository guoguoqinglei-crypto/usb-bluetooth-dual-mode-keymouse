#include <stdint.h>

#include <tusb.h>
#include <device/dcd.h>

#include <stm32f10x.h>

#include <usb_audio.h>
#include <usb_descriptors.h>
#include <I2C.h>
#include <uart.h>

/* ============================================================================
 * USB 音频扬声器 —— 设备层与音频数据层
 * ----------------------------------------------------------------------------
 * 数据流：
 *
 *   电脑 --USB 等时 OUT--> TinyUSB 内部 FIFO --主循环泵--> 本文件的环形缓冲
 *                                                             |
 *                                        DMA 半传输/完成中断（i2s.c）
 *                                                             v
 *                                                   I²S2 --> NAU88C22 --> 耳机
 *
 * 为什么中间还要再加一层环形缓冲：
 *   TinyUSB 的 FIFO 只有 4 包（4ms），而 I²S 的中断是严格每 1ms 触发一次，
 *   两者的节拍来自两个完全独立的时钟源（USB 48MHz / 晶振 + I²S 分频）。
 *   中间垫一层 1024 帧（约 21ms）的缓冲，才能吸收掉两边累积的相位差。
 *   这个相位差的本源是采样率偏差：电脑严格按 48000 帧/秒发，而我们实际只播
 *   46875 帧/秒，每秒多出 1125 帧，必须有地方存或者丢。
 * ========================================================================== */

/* 环形缓冲长度（单位：帧，1 帧 = 左 + 右各 1 个 int16 = 4 字节）
 * 必须是 2 的幂，这样索引用位与运算回绕，不用做除法。
 * 1024 帧 ≈ 21.8ms，足够吸收 USB 侧的调度抖动。 */
#define RING_FRAMES         1024u
#define RING_MASK           (RING_FRAMES - 1u)

/* 起播水位：缓冲里攒够 256 帧（约 5.5ms）才开始输出，
 * 防止刚开流时缓冲是空的、播一下就断，听感上是一串“哒哒”声。 */
#define START_WATER         256u

/* 每 1ms 主机送来的字节数：48000 ÷ 1000 × 2 声道 × 2 字节 = 192 */
#define PKT_BYTES           192u

/* 一包对应的帧数：192 ÷ 4 = 48 */
#define PKT_FRAMES          (PKT_BYTES / 4u)

/* 特征单元编号，必须与描述符里的 bUnitID 一致（usb_descriptors.c 里写的是 2） */
#define AUDIO_FEATURE_UNIT_ID   2u

/* 控制选择子，UAC 1.0 规范定义：0x01 静音，0x02 音量 */
#define UAC_CTRL_MUTE       0x01u
#define UAC_CTRL_VOLUME     0x02u

/* 音量单位是 1/256 dB：
 *   0x7FFF = +127.99dB，0x0000 = 0dB，0x8000 = -128dB（静音）
 * 这里把对外声明的范围限制在 -96dB ~ 0dB，跟普通声卡一致，
 * 否则 Windows 的音量滑块会出现 +127dB 这种荒唐的上限。 */
#define UAC_VOL_MIN         (-96 * 256)
#define UAC_VOL_MAX         (0)
#define UAC_VOL_RES         (1)

/*--------------------------------------------------------------------+
 * 环形缓冲
 *--------------------------------------------------------------------*/
static int16_t s_ring[RING_FRAMES * 2u];

/* 写指针由主循环推进，读指针由 DMA 中断推进。
 * 16 位访问在 Cortex-M3 上是原子的，单生产/单消费场景不需要关中断。 */
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

/* 主机是否已切到 alt 1（开流） */
static volatile uint8_t  s_alt;
/* 是否已经开始输出 USB 数据（一旦开始就不再退回本地音效，避免中途跳变） */
static volatile uint8_t  s_started;

static volatile uint32_t s_overflow;     /* 缓冲满、丢帧的次数 */
static volatile uint32_t s_underrun;     /* 缓冲空、补静音的次数 */
static volatile uint32_t s_rx_bytes;     /* 累计从 USB 收到的字节数，用于判断主机到底有没有在送数据 */
static volatile uint32_t s_alt_changes;  /* 主机切换 alt 的次数，用于判断是不是主机反复开关流 */

/* 主机请求的音量/静音，先记下来，由主循环去写 Codec。
 * 不在 USB 中断里直接写 I²C：软件 I²C 靠延时循环产生 400kHz 时序，
 * 放进中断会拉长中断时间，干扰 USB 的 1ms 节奏。 */
static volatile uint8_t  s_usb_mute;
static volatile int16_t  s_usb_volume = (int16_t)UAC_VOL_MIN;
static volatile uint8_t  s_volume_dirty;

/*--------------------------------------------------------------------+
 * 设备层：时钟、时基、中断
 *--------------------------------------------------------------------*/

/* TinyUSB 在无 RTOS 模式下要求应用提供毫秒时基 */
static volatile uint32_t s_millis = 0;

uint32_t tusb_time_millis_api(void)
{
    return s_millis;
}

/* 协议栈与主循环统一用这个接口读时基。
 * 之前只在 bt_hci.h 里声明过、没有实现，属于一个悬空的外部符号，
 * 这里补上定义，让 s_millis 只有一个出口。 */
uint32_t hal_time_ms(void)
{
    return s_millis;
}

/* TIM4 产生 1ms 中断，只做计数，不占用 SysTick（SysTick 被 Delay 驱动占用） */
void TIM4_IRQHandler(void)
{
    if ((TIM4->SR & TIM_SR_UIF) != 0U)
    {
        TIM4->SR = (uint16_t)~TIM_SR_UIF;
        s_millis++;
    }
}

/* 1ms 时基是否已启动。做手柄时 USB 音频整条链路是关的，
 * 但 BTstack 和主循环都还在用 hal_time_ms()，所以这个时基必须能独立启动，
 * 不能只在 UsbAudio_Init() 里初始化。做成幂等的，两边都能调。 */
static uint8_t s_timebase_started = 0;

void UsbAudio_TimeBaseInit(void)
{
    if (s_timebase_started != 0U)
    {
        return;
    }

    s_timebase_started = 1U;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    /* TIM4 时钟 = 72MHz，72 分频后 1MHz，计 1000 个数为 1ms */
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

void UsbAudio_PreInit(void)
{
    GPIO_InitTypeDef gpio;

    /* PA12(D+) 输出低电平：外部 1.5k 上拉被拉低，主机认为设备未插入。
     * Codec 初始化要几百毫秒，期间没有 tud_task() 响应枚举，先“不插”
     * 可以避免主机枚举超时后留下一个无法识别的设备。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(GPIOA, GPIO_Pin_12);
}

void UsbAudio_Init(void)
{
    GPIO_InitTypeDef gpio;
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };

    /* 第 1 步：先选好 USB 时钟分频（72MHz / 1.5 = 48MHz），再打开 USB 外设时钟。
     * RM0008 明确要求：USBPRE 必须在 USBEN 置位之前配置好。 */
    RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

    /* 第 2 步：PA11 = USB_DM，PA12 = USB_DP，复用推挽输出 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_11 | GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    /* 第 3 步：启动 1ms 时基，并设置 USB 中断优先级 */
    UsbAudio_TimeBaseInit();
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 2);
    NVIC_SetPriority(USB_HP_CAN1_TX_IRQn, 2);

    /* 第 4 步：启动 TinyUSB 设备协议栈 */
    tusb_init(0, &dev_init);
}

/*--------------------------------------------------------------------+
 * 音频数据层
 *--------------------------------------------------------------------*/

/* 当前缓冲里有多少帧（head 追过 tail 时用位与回绕） */
uint16_t UsbAudio_BufferedFrames(void)
{
    return (uint16_t)((s_head - s_tail) & RING_MASK);
}

uint32_t UsbAudio_OverflowCount(void)
{
    return s_overflow;
}

uint32_t UsbAudio_UnderrunCount(void)
{
    return s_underrun;
}

/* 把主机来的数据压进环形缓冲；缓冲满时丢掉最旧的一帧。
 *
 * 为什么丢"最旧"而不是丢"新来的"：
 *   电脑每秒发 48000 帧，我们每秒只播 46875 帧，多出来的 1125 帧必须处理掉。
 *   如果整批丢新帧，一次会丢掉一整个 1ms 的包，每秒丢 23 次，
 *   听感是"哒、哒、哒"的连续卡顿；
 *   丢最旧的一帧则是把 1125 帧均匀摊开，每次只跳过一个样本（20.8µs），
 *   听感上只是极轻微的粗糙感，不会出现明显的断音。
 *
 * 唯一需要注意的是：这里会改 s_tail，而 s_tail 平时只由 DMA 中断改。
 * 两处都是"读-改-写"，必须短暂关掉该中断，否则指针错位就会出现
 * "播着播着停住、过一会儿又自己接上"这种玄学现象。 */
static void UsbAudio_Push(const int16_t *src, uint32_t frames)
{
    uint32_t i;

    for (i = 0; i < frames; i++)
    {
        uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);

        if (next == s_tail)
        {
            /* 缓冲满：丢弃最旧的一帧，为当前帧腾位置。
             * 关中断只有 3 条指令的窗口，对 USB 时序没有影响。 */
            NVIC_DisableIRQ(DMA1_Channel5_IRQn);
            s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
            NVIC_EnableIRQ(DMA1_Channel5_IRQn);
            s_overflow++;
        }

        s_ring[s_head * 2u]      = src[i * 2u];        /* 左声道 */
        s_ring[s_head * 2u + 1u] = src[i * 2u + 1u];   /* 右声道 */
        s_head = next;
    }
}

/* ============================================================================
 * 48 kHz -> 46.875 kHz 的软件重采样（线性插值）
 * ----------------------------------------------------------------------------
 * 问题的来源：
 *   电脑严格按 48000 帧/秒发；I²S 受 72MHz 整数分频限制只能播 46875 帧/秒。
 *   每秒多出 1125 帧，必须处理掉。
 *
 * 之前怎么做的、为什么不行：
 *   直接"丢掉最旧的一帧"。丢的动作相当于把波形剪掉一小段再接上，
 *   每一次都是一次阶跃（20.8µs 的突变）。每秒 1125 次阶跃，
 *   有声音的时候听起来就是连续的"哒哒哒"，人声最明显 ——
 *   因为人声有大量能量集中在中低频，对这种周期性突变最敏感。
 *
 * 现在的做法：
 *   不丢，而是"多吞一点"。读指针不再每次走 1 帧，而是走 1.024 帧：
 *   落在两个采样点中间时，用线性插值算出中间值。
 *   波形始终连续，没有阶跃，所以不会有"哒"。
 *
 * 为什么音调不变（这一点容易想错）：
 *   看的是"每秒吞掉多少输入帧"，不是"每帧走多远"：
 *     输出 46875 帧/秒 × 每帧 1.024 输入帧 = 48000 输入帧/秒
 *   恰好等于电脑送来的速率，所以既没有积压也没有欠载，
 *   播放速度和音调都与原始文件一致。
 *   反过来，如果每帧只走 1.0 帧，就只吞 46875 帧/秒，
 *   缓冲会一直涨到溢出——那才是老师课上说的"错误示范"。
 *
 * 代价（必须知道）：
 *   严格来说降采样前应加低通滤波（抗混叠），这里省掉了，
 *   高频段会有一点混叠失真 —— 但比每秒 1125 次爆音好得多。
 *   另外本文件开头"128 帧转 125 帧"就是这个 1.024 的另一种写法：
 *   48000 × 125 / 128 = 46875，两者完全等价。
 *
 * 数值：
 *   48000 / 46875 = 1.024
 *   用 16.16 定点表示：1.024 × 65536 = 67108.86 -> 取 67109
 *   每输出一帧，小数部分累加 67109 - 65536 = 1573；
 *   累加到 65536 就多吞一帧，平均每 42.7 帧吞一次，正好是 2.34%。
 * ========================================================================== */

#define RESAMPLE_STEP     67109u    /* 1.024 的 16.16 定点表示 */
#define RESAMPLE_UNIT     65536u    /* 16.16 的 1.0 */

/* 分数指针的小数部分，必须跨调用保持，否则每个半区都会重新对齐，
 * 累积误差会变成周期性的"噗噗"声。 */
static uint32_t s_frac;

/* 从环形缓冲取 frames 个立体声帧，内部完成 48k -> 46.875k 的重采样。
 * 返回值是实际输出的帧数，小于 frames 说明欠载，调用方补静音。 */
uint32_t UsbAudio_ReadResampled(int16_t *dst, uint32_t frames)
{
    uint32_t n = 0;
    uint32_t avail;

    while (n < frames)
    {
        avail = (uint16_t)((s_head - s_tail) & RING_MASK);

        if (avail == 0u)
        {
            break;                      /* 缓冲空，交给调用方补 0 */
        }

        if (avail >= 2u)
        {
            /* 取当前帧和下一帧做插值 */
            uint16_t i0 = s_tail;
            uint16_t i1 = (uint16_t)((s_tail + 1u) & RING_MASK);
            int32_t  l0 = (int32_t)s_ring[i0 * 2u];
            int32_t  l1 = (int32_t)s_ring[i1 * 2u];
            int32_t  r0 = (int32_t)s_ring[i0 * 2u + 1u];
            int32_t  r1 = (int32_t)s_ring[i1 * 2u + 1u];

            /* 用 64 位乘法：差值最大 65535，乘 65535 会超出 int32 范围 */
            dst[n * 2u]      = (int16_t)(l0 + (int32_t)(((int64_t)(l1 - l0) * (int64_t)s_frac) >> 16));
            dst[n * 2u + 1u] = (int16_t)(r0 + (int32_t)(((int64_t)(r1 - r0) * (int64_t)s_frac) >> 16));
        }
        else
        {
            /* 只剩最后一帧，没有"下一帧"可插值，直接输出 */
            dst[n * 2u]      = (int16_t)s_ring[s_tail * 2u];
            dst[n * 2u + 1u] = (int16_t)s_ring[s_tail * 2u + 1u];
        }

        n++;

        /* 读指针前进 1 帧，小数部分前进 0.024 帧 */
        s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
        s_frac += (RESAMPLE_STEP - RESAMPLE_UNIT);

        /* 小数部分攒满 1 帧：多吞一帧，实现"跳过一个采样点" */
        if (s_frac >= RESAMPLE_UNIT)
        {
            s_frac -= RESAMPLE_UNIT;

            if (s_tail != s_head)
            {
                s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
            }
        }
    }

    if (n < frames)
    {
        s_underrun++;
    }

    return n;
}

/* 不做重采样、直接按原速取帧（保留给调试对比用，正常路径不用） */
uint32_t UsbAudio_ReadFrames(int16_t *dst, uint32_t frames)
{
    uint32_t n = 0;

    while ((n < frames) && (s_tail != s_head))
    {
        dst[n * 2u]      = s_ring[s_tail * 2u];
        dst[n * 2u + 1u] = s_ring[s_tail * 2u + 1u];
        s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
        n++;
    }

    if (n < frames)
    {
        s_underrun++;
    }

    return n;
}

uint8_t UsbAudio_IsStreaming(void)
{
    /* 主机没开流：走本地固定音效 */
    if (s_alt == 0u)
    {
        return 0u;
    }

    /* 已经起播：保持走 USB 通路，欠载时由调用方补静音。
     * 这里不因为缓冲一时空就退回本地音效，否则会在两者之间反复跳。 */
    if (s_started != 0u)
    {
        return 1u;
    }

    /* 首次起播要等水位，避免开流瞬间断续 */
    if (UsbAudio_BufferedFrames() >= START_WATER)
    {
        s_started = 1u;
        return 1u;
    }

    return 0u;
}

/* 主机没开流时，要不要继续播本地固定音效？
 *
 * 置 1（推荐）：只要已经枚举成 USB 声卡，就保持静音。
 *   电脑暂停、切歌、关播放器时，耳机里安安静静，符合"这是一块声卡"的预期。
 * 置 0：没开流就回到本地音效。
 *   这个行为只在"单独演示 30 分那项（不接电脑、只听音效）"时才有用，
 *   接上电脑会让两个音源互相抢，听起来像"音乐停了又自己接着放"。
 */
#define AUDIO_LOCAL_EFFECT_ONLY_WITHOUT_USB   1

uint8_t UsbAudio_AllowLocalEffect(void)
{
#if AUDIO_LOCAL_EFFECT_ONLY_WITHOUT_USB
    /* 已经枚举成声卡：不再播本地音效，让主机来决定放什么 */
    return (tud_audio_mounted() == false) ? 1u : 0u;
#else
    return 1u;
#endif
}

/* 复位缓冲与状态（主机切 alt 时调用） */
static void UsbAudio_ResetBuffer(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_started = 0u;
    s_frac = 0u;        /* 分数指针一起归零，避免下次开流从半帧处开始 */
}

/* 把主机 FIFO 里的数据搬到环形缓冲。
 * 每次只搬完整的帧（4 字节），不足一帧的零头留在 FIFO 里等下次，
 * 否则左右声道会错位，听起来是持续的“沙沙”声。 */
static void UsbAudio_Pump(void)
{
    int16_t  tmp[PKT_FRAMES * 2];      /* 192 字节 */
    uint16_t avail;

    if ((tud_audio_mounted() == false) || (s_alt == 0u))
    {
        return;
    }

    avail = tud_audio_available();

    while (avail >= 4u)
    {
        uint16_t chunk = (avail > PKT_BYTES) ? (uint16_t)PKT_BYTES : avail;
        uint16_t got;

        chunk = (uint16_t)((chunk / 4u) * 4u);
        got   = tud_audio_read(tmp, chunk);

        if (got < 4u)
        {
            break;
        }

        s_rx_bytes += got;
        UsbAudio_Push(tmp, (uint32_t)(got / 4u));
        avail = (uint16_t)(avail - got);
    }
}

/* 把主机音量应用到 Codec（只在主循环里调用，不在中断里做 I²C） */
static void UsbAudio_ApplyVolume(void)
{
    int32_t db;
    int32_t code;
    uint8_t percent;

    if (s_volume_dirty == 0u)
    {
        return;
    }
    s_volume_dirty = 0u;

    if (s_usb_mute != 0u)
    {
        NAU8822_SetVolume(0u);
        return;
    }

    /* 1/256 dB 定点数换算成整数 dB */
    db = (int32_t)s_usb_volume / 256;

    /* NAU8822 耳机增益范围 -57dB ~ +6dB，对应寄存器 0x00 ~ 0x3F */
    code = db + 57;
    if (code < 0)
    {
        code = 0;
    }
    if (code > 63)
    {
        code = 63;
    }

    /* NAU8822_SetVolume 的入参是 0~100 百分比，内部再映射回 0~63 */
    percent = (uint8_t)(code * 100 / 63);
    NAU8822_SetVolume(percent);
}

void UsbAudio_Task(void)
{
    /* 第 1 步：处理 USB 协议栈事件（枚举、类请求、等时传输） */
    tud_task();

    /* 第 2 步：把收到的音频搬进环形缓冲 */
    UsbAudio_Pump();

    /* 第 3 步：主机改过音量就写一次 Codec */
    UsbAudio_ApplyVolume();
}

/* 每 5 秒往串口打一行运行状态，用来判断"停一下又继续"到底卡在哪一层。
 *
 * 各字段含义与判断方法：
 *   mnt   1 = 主机已完成配置（枚举成功）      0 = 没枚举上，或者没插电脑
 *   alt   1 = 主机已开流，正在送音频          0 = 主机没开流
 *   chg   主机切换 alt 的次数。这个值一直涨，说明是电脑那边反复开关流
 *   rx    累计收到的 KB。不动，说明一个字节都没收到（问题在 USB 侧）
 *   buf   环形缓冲里的帧数（满 1023，约 21.8ms）。
 *         重采样生效时它应该稳定在几百帧，既不持续涨也不持续跌 ——
 *         这就是老师课上"正确示范"的判据：生产速率 = 消费速率。
 *   ovf   缓冲满而丢帧的累计次数。正常应**一直不动**（除了刚开流那一下）。
 *         如果它每秒涨 1000 左右，说明重采样没真正生效（每帧只吞 1 帧输入），
 *         缓冲会一路堆到满，听感上就是周期性爆音。
 *   ur    缓冲空而补静音的累计次数。这个值一涨，听感上就是"停了一下"，
 *         说明给数据的速度跟不上播音的速度
 */

/* 诊断打印的统计窗口（毫秒）
 * 为什么不按 1 秒打：UART1_Printf 是阻塞发送，一行 95 字节在 115200 下
 * 要占主循环约 8ms，这期间 tud_task() 停摆，等时端点必然掉包，反而会
 * 制造出"每秒顿一下"的假故障。放宽到 5 秒打印一次，干扰降到原来的 1/5；
 * 波特率提到 921600 后单次只占约 1ms，双重保险。 */
#define DIAG_WINDOW_MS      5000u
#define DIAG_WINDOW_S       (DIAG_WINDOW_MS / 1000u)

void UsbAudio_DiagTask(void)
{
    static uint32_t last_ms = 0;
    static uint32_t last_rx = 0;
    uint32_t now = s_millis;
    uint32_t rx_now;
    uint32_t rx_delta;

    if ((uint32_t)(now - last_ms) < DIAG_WINDOW_MS)
    {
        return;
    }
    last_ms = now;

    /* 窗口内实际收到的字节数，除以 5 换算成"每秒多少字节"。
     * 正常播放时应约等于 192000（48000 帧 × 2 声道 × 2 字节），
     * 空闲时接近 0。作业要求"打印 1s 的数据量"，看这个字段即可。 */
    rx_now   = s_rx_bytes;
    rx_delta = rx_now - last_rx;
    last_rx  = rx_now;

    UART1_Printf("[usb] mnt=%u alt=%u chg=%lu 1s=%luB rx=%luKB buf=%u ovf=%lu ur=%lu\r\n",
                 (unsigned)tud_audio_mounted(),
                 (unsigned)s_alt,
                 (unsigned long)s_alt_changes,
                 (unsigned long)(rx_delta / DIAG_WINDOW_S),
                 (unsigned long)(rx_now >> 10),
                 (unsigned)UsbAudio_BufferedFrames(),
                 (unsigned long)s_overflow,
                 (unsigned long)s_underrun);
}

/*--------------------------------------------------------------------+
 * USB 音频类回调
 *--------------------------------------------------------------------*/

/* 主机切换 alt setting：alt 1 = 开始播放，alt 0 = 停止 */
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t itf = (uint8_t)(p_request->wIndex & 0xFFu);
    uint8_t alt = (uint8_t)(p_request->wValue & 0xFFu);

    (void)rhport;

    if (itf == ITF_NUM_AUDIO_STREAMING)
    {
        UsbAudio_ResetBuffer();
        s_alt = (alt != 0u) ? 1u : 0u;
        s_alt_changes++;
    }

    return true;
}

/* 主机关闭端点（停止播放）时被调用 */
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;

    s_alt = 0u;
    UsbAudio_ResetBuffer();
    return true;
}

/* 主机 SET_CUR：音量、静音 */
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    uint8_t unitID  = (uint8_t)(p_request->wIndex >> 8);
    uint8_t ctrlSel = (uint8_t)(p_request->wValue >> 8);

    (void)rhport;

    if (unitID != AUDIO_FEATURE_UNIT_ID)
    {
        return false;
    }

    if (p_request->bRequest != (uint8_t)AUDIO10_CS_REQ_SET_CUR)
    {
        return true;   /* 其它请求（如 SET_MIN）直接应答，不做处理 */
    }

    if (ctrlSel == UAC_CTRL_MUTE)
    {
        s_usb_mute = (pBuff[0] != 0u) ? 1u : 0u;
        s_volume_dirty = 1u;
        return true;
    }

    if (ctrlSel == UAC_CTRL_VOLUME)
    {
        /* 16 位有符号，低字节在前 */
        s_usb_volume = (int16_t)((uint16_t)pBuff[0] | ((uint16_t)pBuff[1] << 8));
        s_volume_dirty = 1u;
        return true;
    }

    return false;
}

/* 主机 GET_CUR / GET_MIN / GET_MAX / GET_RES：读音量、静音 */
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t unitID  = (uint8_t)(p_request->wIndex >> 8);
    uint8_t ctrlSel = (uint8_t)(p_request->wValue >> 8);
    uint8_t reqCode = p_request->bRequest;

    if (unitID != AUDIO_FEATURE_UNIT_ID)
    {
        return false;
    }

    if (ctrlSel == UAC_CTRL_MUTE)
    {
        uint8_t mute = s_usb_mute;

        switch (reqCode)
        {
            case AUDIO10_CS_REQ_GET_CUR:
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
            default:
                /* 静音没有最小/最大/步进的概念，这类请求直接 STALL 才是规范做法。
                 * 注意不能返回 true 了事：true 表示“已处理”，但没发起数据传输，
                 * 控制传输会一直挂着，主机会等到超时。 */
                return false;
        }
    }

    if (ctrlSel == UAC_CTRL_VOLUME)
    {
        int16_t value;

        switch (reqCode)
        {
            case AUDIO10_CS_REQ_GET_CUR:
                value = s_usb_volume;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &value, sizeof(value));

            case AUDIO10_CS_REQ_GET_MIN:
                value = (int16_t)UAC_VOL_MIN;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &value, sizeof(value));

            case AUDIO10_CS_REQ_GET_MAX:
                value = (int16_t)UAC_VOL_MAX;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &value, sizeof(value));

            case AUDIO10_CS_REQ_GET_RES:
                value = (int16_t)UAC_VOL_RES;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &value, sizeof(value));

            default:
                return false;
        }
    }

    return false;
}

/*--------------------------------------------------------------------+
 * USB 中断入口
 *--------------------------------------------------------------------*/

void USB_LP_CAN1_RX0_IRQHandler(void)
{
    dcd_int_handler(0);
}

void USB_HP_CAN1_TX_IRQHandler(void)
{
    dcd_int_handler(0);
}

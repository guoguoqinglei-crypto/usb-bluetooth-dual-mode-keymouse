#ifndef USB_AUDIO_H_
#define USB_AUDIO_H_

#include <stdint.h>

/* ============================================================================
 * USB 音频扬声器 —— 设备层 + 音频数据层
 * ----------------------------------------------------------------------------
 * 分两层职责：
 *
 *   设备层（UsbAudio_Init / Task）
 *     打开 USB 时钟（PLL/1.5 = 48MHz）、配置 PA11/PA12、提供 1ms 时基、
 *     把 USB 中断转发给 TinyUSB、主循环里调用 tud_task()。
 *
 *   音频数据层（Push / ReadFrames）
 *     USB 收到的主机音频先进入一个环形缓冲，I²S 的半传输/传输完成中断
 *     再从环形缓冲取数据。两边各取各的，靠读写指针解耦，
 *     因此 USB 侧的 1ms 抖动不会直接影响 I²S 的输出时序。
 * ========================================================================== */

/* 1ms 时基。返回上电以来经过的毫秒数（TIM4 中断累加，溢出约 49 天）。
 * 蓝牙协议栈的超时判断、LED 闪烁节拍、手柄报告周期都读这个值。
 * 必须先调用 UsbAudio_TimeBaseInit()，否则永远返回 0。 */
uint32_t hal_time_ms(void);

/* 上电早期调用：把 D+（PA12）拉低，让主机暂时看不到设备。
 * 必须放在 main() 最前面，否则 Codec 初始化那几百毫秒里主机已经开始枚举，
 * 会因为设备不应答而在设备管理器里留下一个“未知设备”。 */
void UsbAudio_PreInit(void);

/* 启动 1ms 时基（TIM4 + 中断，只做计数）。
 * 幂等，可以重复调用。
 * 注意：即使关掉 USB 音频（ENABLE_USB_AUDIO = 0），这个也必须调用——
 * BTstack 的超时判断和主循环里的定时逻辑都读 hal_time_ms()，
 * 而 hal_time_ms() 就是读这里计的数，不启动它时间永远停在 0。
 * UsbAudio_Init() 内部也会调一次，所以开 USB 音频时不用手动加。 */
void UsbAudio_TimeBaseInit(void);

/* 初始化 USB 时钟、引脚、1ms 时基并启动 TinyUSB 协议栈 */
void UsbAudio_Init(void);

/* TinyUSB 设备任务 + 音频数据泵，必须放在主循环里反复调用 */
void UsbAudio_Task(void);

/* 每秒往串口打一行运行状态（mnt/alt/chg/rx/buf/ovf/ur），
 * 用于判断"突然停一下又继续"是主机没送数据、还是缓冲欠载。 */
void UsbAudio_DiagTask(void);

/* 主机是否已经开流（切到 alt 1 且缓冲水位够）。
 * 返回 1：I²S 应该取 USB 的数据；返回 0：继续播本地固定音效。
 * 该函数会被 DMA 中断每 1ms 调用一次，内部只做标志判断，不阻塞。 */
uint8_t UsbAudio_IsStreaming(void);

/* 当前是否允许播本地固定音效。
 * 返回 0 表示设备已经枚举成 USB 声卡，应该输出静音等待主机送数据，
 * 而不是回头播 sound_effect.h 里的音效，否则电脑一暂停就会响起音效。
 * 该函数会被 DMA 中断调用，内部只读一个标志。 */
uint8_t UsbAudio_AllowLocalEffect(void);

/* 从环形缓冲取 frames 个立体声帧（每帧 = 左 1 个 int16 + 右 1 个 int16）。
 * 返回值是实际取到的帧数，小于 frames 说明欠载，调用方需要补静音。 */
uint32_t UsbAudio_ReadFrames(int16_t *dst, uint32_t frames);

/* 同上，但内部完成 48kHz -> 46.875kHz 的线性插值重采样。
 * 正常播放路径必须用这个：直接取帧会在每秒丢掉 1125 个样本，
 * 每次丢弃都是一次波形阶跃，听感上就是持续的"哒哒"声。
 * 该函数会被 DMA 中断调用，内部只用整数运算，不阻塞。 */
uint32_t UsbAudio_ReadResampled(int16_t *dst, uint32_t frames);

/* 当前缓冲里已缓存的帧数（1 帧 = 4 字节 = 1ms/48） */
uint16_t UsbAudio_BufferedFrames(void);

/* 统计量，用于调试：溢出说明电脑发得比 I²S 播得快，欠载反之 */
uint32_t UsbAudio_OverflowCount(void);
uint32_t UsbAudio_UnderrunCount(void);

#endif /* USB_AUDIO_H_ */

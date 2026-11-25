#include <delay.h>
#include <stm32f10x.h>

/* ============================================================================
 * 延时函数（FreeRTOS 版本）
 * ----------------------------------------------------------------------------
 * 【为什么改成 TIM7】
 *   原来 Delay_us() 直接操作 SysTick（内核的 24 位倒数定时器）。
 *   接入 FreeRTOS 之后 SysTick 被内核独占（作为 1ms 调度节拍），
 *   再在应用里改 SysTick->LOAD/CTRL 会直接破坏调度器的时间基准。
 *   所以这里改用空闲的基本定时器 TIM7 做微秒级延时：
 *     TIM7 挂在 APB1，计数时钟 72MHz，预分频 72 → 1 个计数 = 1 微秒
 *
 * 【使用注意】
 *   接入 RTOS 后，任务里需要"等待"时应该用 vTaskDelay()（让出 CPU），
 *   Delay_ms()/Delay_s() 是【阻塞式】的，只建议在调度器启动前的
 *   外设初始化（Codec、I2S 等）里使用。
 * ========================================================================== */

static uint8_t s_tim7Ready = 0U;

/* TIM7 只需要初始化一次（预分频固定为 1us 计数） */
static void tim7_init_once(void)
{
    if (s_tim7Ready != 0U)
    {
        return;
    }
    s_tim7Ready = 1U;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM7, ENABLE);

    TIM7->CR1 = 0U;                 /* 先关掉，配置期间不计数 */
    TIM7->PSC = 72U - 1U;           /* 72MHz / 72 = 1MHz → 1 计数 = 1us */
    TIM7->ARR = 0xFFFFU;
    TIM7->CNT = 0U;
    TIM7->EGR = TIM_EGR_UG;         /* 立即把 PSC 装入影子寄存器 */
    TIM7->SR  = 0U;
}

/* 微秒级延时（阻塞） */
void Delay_us(uint16_t us)
{
    if (us == 0U)
    {
        return;
    }

    tim7_init_once();

    TIM7->CR1 = 0U;                            /* 停表 */
    TIM7->CNT = 0U;
    TIM7->ARR = (uint16_t)(us - 1U);           /* 计 us 个数就是 us 微秒 */
    TIM7->EGR = TIM_EGR_UG;                    /* 立即装载新的 ARR */
    TIM7->SR  = 0U;                            /* 清更新标志 */
    TIM7->CR1 = TIM_CR1_CEN;                   /* 启动 */

    while ((TIM7->SR & TIM_SR_UIF) == 0U)
    {
        /* 轮询等待（不占中断资源） */
    }

    TIM7->CR1 = 0U;                            /* 停表 */
    TIM7->SR  = 0U;
}

/* 毫秒级延时（阻塞） */
void Delay_ms(uint16_t ms)
{
    while (ms != 0U)
    {
        Delay_us(1000U);
        ms--;
    }
}

/* 秒级延时（阻塞） */
void Delay_s(uint16_t s)
{
    while (s != 0U)
    {
        Delay_ms(1000U);
        s--;
    }
}

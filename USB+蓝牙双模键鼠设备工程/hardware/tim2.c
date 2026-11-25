#include <tim2.h>

#include <stm32f10x.h>

// 软件 PWM 的周期级数：100 级对应 0~100%
#define TIM2_PWM_STEPS 100U

// 三路 LED 占空比和软件 PWM 计数
static volatile uint8_t pwmRedDuty = 0;
static volatile uint8_t pwmGreenDuty = 0;
static volatile uint8_t pwmBlueDuty = 0;
static volatile uint8_t pwmCounter = 0;

void Tim2_Init(void)
{
    // 第 1 步：开启 GPIOC 和 TIM2 时钟
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

    // 第 2 步：把 PC7、PC8、PC9 配置为通用推挽输出
    GPIOC->CRL &= ~(GPIO_CRL_MODE7 | GPIO_CRL_CNF7);
    GPIOC->CRL |= GPIO_CRL_MODE7;

    GPIOC->CRH &= ~(GPIO_CRH_MODE8 | GPIO_CRH_CNF8);
    GPIOC->CRH |= GPIO_CRH_MODE8;

    GPIOC->CRH &= ~(GPIO_CRH_MODE9 | GPIO_CRH_CNF9);
    GPIOC->CRH |= GPIO_CRH_MODE9;

    // 第 3 步：初始亮度全部为 0
    pwmRedDuty = 0;
    pwmGreenDuty = 0;
    pwmBlueDuty = 0;
    pwmCounter = 0;

    // 第 4 步：72MHz / (71 + 1) = 1MHz，即每个计数 1us
    TIM2->PSC = 71;
    TIM2->ARR = TIM2_PWM_STEPS - 1U;
    TIM2->CNT = 0;

    // 第 6 步：使能更新中断
    TIM2->DIER |= TIM_DIER_UIE;

    // 第 7 步：配置 NVIC，使用统一的中断优先级分组
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    NVIC_SetPriority(TIM2_IRQn, 4);
    NVIC_EnableIRQ(TIM2_IRQn);

    // 第 8 步：启动 TIM2
    TIM2->CR1 |= TIM_CR1_CEN;
}

void Tim2_SetRedBrightness(uint8_t brightness)
{
    // 第 1 步：限制亮度范围
    if (brightness > 100U)
    {
        brightness = 100U;
    }
    // 第 2 步：更新红灯占空比
    pwmRedDuty = brightness;
}

void Tim2_SetGreenBrightness(uint8_t brightness)
{
    // 第 1 步：限制亮度范围
    if (brightness > 100U)
    {
        brightness = 100U;
    }
    // 第 2 步：更新绿灯占空比
    pwmGreenDuty = brightness;
}

void Tim2_SetBlueBrightness(uint8_t brightness)
{
    // 第 1 步：限制亮度范围
    if (brightness > 100U)
    {
        brightness = 100U;
    }
    // 第 2 步：更新蓝灯占空比
    pwmBlueDuty = brightness;
}

void TIM2_IRQHandler(void)
{
    // 第 1 步：判断并清除更新中断标志
    if (TIM2->SR & TIM_SR_UIF)
    {
        TIM2->SR &= ~TIM_SR_UIF;

        // 第 2 步：推进软件 PWM 计数
        pwmCounter++;
        if (pwmCounter >= TIM2_PWM_STEPS)
        {
            pwmCounter = 0;
        }

        // 第 3 步：根据占空比控制红灯 PC7
        if (pwmCounter < pwmRedDuty)
        {
            GPIOC->BSRR = GPIO_Pin_7;
        }
        else
        {
            GPIOC->BRR = GPIO_Pin_7;
        }

        // 第 4 步：根据占空比控制绿灯 PC8
        if (pwmCounter < pwmGreenDuty)
        {
            GPIOC->BSRR = GPIO_Pin_8;
        }
        else
        {
            GPIOC->BRR = GPIO_Pin_8;
        }

        // 第 5 步：根据占空比控制蓝灯 PC9
        if (pwmCounter < pwmBlueDuty)
        {
            GPIOC->BSRR = GPIO_Pin_9;
        }
        else
        {
            GPIOC->BRR = GPIO_Pin_9;
        }
    }
}

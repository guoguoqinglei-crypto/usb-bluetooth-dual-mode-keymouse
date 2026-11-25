#include <Init_rda5876.h>

#include <stm32f10x.h>
#include <string.h>

#include <delay.h>
#include <bt_uart.h>
#include <uart.h>           /* UART1_Printf / UART1_SendString（诊断打印） */
#include <RDA5876_INTI.h>

volatile uint8_t rda_stage = 0U;

#define RDA_CMD_H4      0x01U
#define RDA_VENDOR_OP1  0x02U
#define RDA_VENDOR_OP2  0xFDU
#define RDA_CMD_PLEN    0x0AU

#define RDA_MEM_80      0x80U       /* 第一组之后切到 0x81 */
#define RDA_MEM_81      0x81U
#define RDA_MEM_SEG     0x01U

#define RDA_TABLE_END   0xAAU       /* 每张表的结束标记 */

/* PC12：高 = 释放复位 + LDO 开；低 = 复位 + 断电 */
static void rda_pc12(uint8_t high)
{
    if (high != 0U)
    {
        GPIO_SetBits(GPIOC, GPIO_Pin_12);
    }
    else
    {
        GPIO_ResetBits(GPIOC, GPIO_Pin_12);
    }
}

/* PA8 = TIM1_CH1 输出 32.768kHz 方波。
 * 72MHz / 2197 = 32772Hz，误差 0.01%，任何晶振都吃得下。 */
static void rda_32k_on(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1 | RCC_APB2Periph_GPIOA, ENABLE);

    /* PA8 复用推挽 */
    GPIOA->CRH &= ~(uint32_t)(GPIO_CRH_MODE8 | GPIO_CRH_CNF8);
    GPIOA->CRH |=  (uint32_t)(GPIO_CRH_MODE8 | GPIO_CRH_CNF8_1);

    TIM1->PSC  = 0U;
    TIM1->ARR  = 2196U;
    TIM1->CCR1 = 1098U;                 /* 50% 占空比 */

    /* PWM 模式 1 */
    TIM1->CCMR1 &= ~(uint32_t)(TIM_CCMR1_CC1S | TIM_CCMR1_OC1M);
    TIM1->CCMR1 |=  (uint32_t)(TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1);

    TIM1->CCER &= ~(uint32_t)TIM_CCER_CC1P;
    TIM1->CCER |=  (uint32_t)TIM_CCER_CC1E;

    TIM1->BDTR |= TIM_BDTR_MOE;         /* 高级定时器必须开主输出 */
    TIM1->CR1  |= TIM_CR1_ARPE;
    TIM1->EGR   = TIM_EGR_UG;
    TIM1->CR1  |= TIM_CR1_CEN;
}

/* 发一条 14 字节的寄存器写命令。
 * buf[4..5] 是 memType + 段号，buf[6..9] 是地址，buf[10..13] 是数据。 */
static void rda_write14(const uint8_t *body8, uint8_t memType)
{
    uint8_t buf[14];

    buf[0]  = RDA_CMD_H4;
    buf[1]  = RDA_VENDOR_OP1;
    buf[2]  = RDA_VENDOR_OP2;
    buf[3]  = RDA_CMD_PLEN;
    buf[4]  = memType;
    buf[5]  = RDA_MEM_SEG;
    memcpy(&buf[6], body8, 8U);

    BtUart_Send(buf, 14U);
}

static void rda_send_ps(uint8_t doReset)
{
    uint8_t  memType = RDA_MEM_80;
    uint16_t i       = 0U;

    for (;;)
    {
        Delay_ms(4);                    /* 厂商每条之间 Delayms(4) */

        if ((i >= (uint16_t)sizeof(rad5876_init_ps)) ||
            (rad5876_init_ps[i] == RDA_TABLE_END))
        {
            if (doReset != 0U)
            {
                Delay_ms(50);
                rda_pc12(0U);
                Delay_ms(100);
                rda_pc12(1U);
                Delay_ms(100);
            }
            break;
        }

        rda_write14(&rad5876_init_ps[i], memType);
        i       = (uint16_t)(i + 8U);
        memType = RDA_MEM_81;
    }
}

/* rad5876_init_0 / rad5876_init_2：整张表都用 memType = 0x80，不切 0x81。
 * 这是厂商 RDA5876_Set_Reg() 里的原始行为，不要"顺手改好"。 */
static void rda_send_table80(const unsigned char *tbl, uint16_t size)
{
    uint16_t i = 0U;

    for (;;)
    {
        Delay_ms(4);

        if ((i >= size) || (tbl[i] == RDA_TABLE_END))
        {
            break;
        }

        rda_write14(&tbl[i], RDA_MEM_80);
        i = (uint16_t)(i + 8U);
    }
}

/* rad5876_init_1：10 字节，连发两遍，第二遍把第 7 个数据字节改成 0x2B。
 * 厂商原代码就是 memcpy 到 buf[4] 再覆盖 buf[10]，照抄。 */
static void rda_send_init1(void)
{
    uint8_t buf[14];

    buf[0] = RDA_CMD_H4;
    buf[1] = RDA_VENDOR_OP1;
    buf[2] = RDA_VENDOR_OP2;
    buf[3] = RDA_CMD_PLEN;
    memcpy(&buf[4], &rad5876_init_1[0], 10U);

    BtUart_Send(buf, 14U);

    Delay_ms(4);
    buf[10] = 0x2BU;                    /* 厂商注释里被注释掉的那一行 */
    BtUart_Send(buf, 14U);
}

/* 五张表，顺序与 RDA5876_Set_Reg() 完全一致 */
static void rda_load_patch(void)
{
    rda_send_ps(1U);                                    /* SET_SAME_CMD 第一遍 + 核心复位 */
    rda_send_ps(0U);                                    /* SET_SAME_CMD 第二遍 */
    rda_send_table80(rad5876_init_0, (uint16_t)sizeof(rad5876_init_0));
    rda_send_init1();                                   /* rad5876_init_1 ×2 */
    rda_send_table80(rad5876_init_2, (uint16_t)sizeof(rad5876_init_2));

    /* rad5876setok：厂商给的是 7 字节，plen 写 0x02 但后面还跟了一个 0x96。
     * 这是 RDA 自己的收尾命令，原样发，不要"修正"长度字段。 */
    Delay_ms(10);
    BtUart_Send(rad5876setok, (uint16_t)sizeof(rad5876setok));
    Delay_ms(10);
}

/* 把串口里已收到的字节打印出来（会消费掉）。
 * 诊断用：补丁阶段芯片到底说了什么、切成 921600 之后有没有回音。 */
static void rda_dump_rx(const char *tag, uint16_t maxn)
{
    uint8_t  b;
    uint16_t n = 0u;

    UART1_Printf("[rda] %s avail=%lu :", tag,
                 (unsigned long)BtUart_Available());

    while ((n < maxn) && (BtUart_TryRead(&b) != 0u))
    {
        UART1_Printf(" %02X", (unsigned)b);
        n++;
    }
    UART1_SendString("\r\n");
}

uint8_t Init_rda5876(void)
{
    GPIO_InitTypeDef gpio;

    rda_stage = 1U;

    /* PC12 推挽输出，先拉低（断电 + 复位） */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    rda_pc12(0U);
    Delay_ms(100);                      /* 厂商 Delayms(100) */

    /* 释放复位。厂商在这里是 PC12 高 -> 开 32k -> 10ms -> 串口 -> 25ms。
     * 顺序别动：芯片一上电就要 32k，晚开会被它当成"没有参考时钟"。 */
    rda_pc12(1U);
    rda_32k_on();
    Delay_ms(10);

    /* 115200 8N1：芯片上电默认参数，补丁只认这一个 */
    rda_stage = 2U;
    BtUart_Init(115200U, USART_Parity_No);
    Delay_ms(25);

    /* 灌五张表 */
    rda_stage = 3U;
    rda_load_patch();

    /* 补丁阶段芯片回了什么 —— 正常情况下它应该一声不吭，
     * 如果这里有字节，说明它是活的，而且我们能收到它 */
    rda_dump_rx("after-patch", 40u);

    rda_stage = 4U;
    BtUart_SetBaud(921600U, USART_Parity_No);

    /* 芯片要跑起 HCI 固件才应答，给足启动时间 */
    Delay_ms(60);

    /* 切完波特率后有没有自己冒出来东西 */
    rda_dump_rx("after-921600", 40u);

    rda_stage = 5U;
    return RDA_OK;
}

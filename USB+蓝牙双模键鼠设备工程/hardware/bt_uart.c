#include <bt_uart.h>

#include <stm32f10x.h>

#include <delay.h>

extern uint32_t hal_time_ms(void);

/* 环形缓冲开 1024：SDP 查询请求只有几十字节，ACL 下行也很小，
 * 这个容量足够覆盖主循环任何一次最长阻塞（比如 Flash 擦除） */
#define BT_UART_RX_BUF_SIZE   1024U
#define BT_UART_RX_BUF_MASK   (BT_UART_RX_BUF_SIZE - 1U)

static volatile uint8_t  s_rxBuf[BT_UART_RX_BUF_SIZE];
static volatile uint16_t s_rxHead = 0U;     /* 中断写 */
static volatile uint16_t s_rxTail = 0U;     /* 主循环读 */
static volatile uint32_t s_rxTotal   = 0U;
static volatile uint32_t s_rxDropped = 0U;
static volatile uint32_t s_txTotal   = 0U;

/* CMSIS 的中断向量靠链接器按名字找，本文件不需要对外暴露；
 * 显式声明一次，避免 -Wmissing-prototypes 报"没有原型"。 */
void USART2_IRQHandler(void);

void USART2_IRQHandler(void)
{
    uint16_t sr = USART2->SR;

    /* RXNE 和 ORE 都靠"读 SR 再读 DR"清除。
     * 溢出时 DR 里还有一个有效字节，一并取走，避免一直卡在溢出状态。 */
    if ((sr & (uint16_t)(USART_SR_RXNE | USART_SR_ORE)) != 0U)
    {
        uint8_t  b    = (uint8_t)(USART2->DR & 0x00FFU);
        uint16_t next = (uint16_t)((s_rxHead + 1U) & BT_UART_RX_BUF_MASK);

        if ((sr & USART_SR_ORE) != 0U)
        {
            s_rxDropped++;                  /* 有字节被硬件丢了，记一笔 */
        }

        if (next != s_rxTail)
        {
            s_rxBuf[s_rxHead] = b;
            s_rxHead = next;
            s_rxTotal++;
        }
        else
        {
            s_rxDropped++;                  /* 缓冲满，这条丢 */
        }
    }
}

static void bt_uart_apply(uint32_t baud, uint16_t parity)
{
    USART_InitTypeDef usart;

    USART_Cmd(USART2, DISABLE);             /* 改参数前必须先关 */

    usart.USART_BaudRate = baud;
    /* 有校验位时，校验位算在字长里：8 数据位 + 1 校验 = 9 位字长。
     * 这一条写错的话，收发双方会各错一位，现象和波特率不对完全一样。 */
    usart.USART_WordLength = (parity == USART_Parity_No)
                                 ? USART_WordLength_8b
                                 : USART_WordLength_9b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = parity;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;

    USART_Init(USART2, &usart);

    USART_Cmd(USART2, ENABLE);

    /* 切换波特率时线上可能有半个字节残留，清掉再开始 */
    (void)USART2->DR;
    (void)USART2->SR;
}

void BtUart_Init(uint32_t baud, uint16_t parity)
{
    GPIO_InitTypeDef gpio;
    NVIC_InitTypeDef nvic;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    /* PA2 = TX */
    gpio.GPIO_Pin   = GPIO_Pin_2;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* PA3 = RX，上拉输入：模块没驱动时保持高电平（UART 空闲态） */
    gpio.GPIO_Pin   = GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &gpio);

    /* PA0 = CTS（接模块 RTS）：不用硬件流控，上拉输入即可 */
    gpio.GPIO_Pin   = GPIO_Pin_0;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &gpio);

    /* PA1 = RTS（接模块 CTS）：固定输出低。
     * CTS 是低有效，拉低等于一直告诉模块"主机随时可以收"。 */
    gpio.GPIO_Pin   = GPIO_Pin_1;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(GPIOA, GPIO_Pin_1);

    bt_uart_apply(baud, parity);

    /* 接收中断从第一刻就打开：补丁阶段芯片只要说话，也一定收得到 */
    nvic.NVIC_IRQChannel                   = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1U;
    nvic.NVIC_IRQChannelSubPriority        = 0U;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
}

void BtUart_SetBaud(uint32_t baud, uint16_t parity)
{
    bt_uart_apply(baud, parity);
    BtUart_Flush();
}

void BtUart_Send(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if ((data == 0) || (len == 0U))
    {
        return;
    }

    for (i = 0U; i < len; i++)
    {
        uint32_t guard = 200000U;

        while ((USART2->SR & USART_SR_TXE) == 0U)
        {
            if (--guard == 0U)
            {
                return;                     /* 卡住就别再等了，交给上层判断 */
            }
        }
        USART2->DR = data[i];
        s_txTotal++;
    }

    /* 等最后一个字节真正移出移位寄存器（TC），
     * 否则紧接着改波特率会把最后一位截断 */
    {
        uint32_t guard = 200000U;
        while ((USART2->SR & USART_SR_TC) == 0U)
        {
            if (--guard == 0U)
            {
                break;
            }
        }
    }
}

uint16_t BtUart_Available(void)
{
    uint16_t head = s_rxHead;
    uint16_t tail = s_rxTail;

    return (uint16_t)((head - tail) & BT_UART_RX_BUF_MASK);
}

uint8_t BtUart_TryRead(uint8_t *out)
{
    if (s_rxTail == s_rxHead)
    {
        return 0U;
    }

    *out     = s_rxBuf[s_rxTail];
    s_rxTail = (uint16_t)((s_rxTail + 1U) & BT_UART_RX_BUF_MASK);
    return 1U;
}

uint8_t BtUart_RxByte(uint8_t *out, uint16_t timeout_ms)
{
    uint32_t t0 = hal_time_ms();

    for (;;)
    {
        if (BtUart_TryRead(out) != 0U)
        {
            return 1U;
        }
        if ((uint32_t)(hal_time_ms() - t0) >= (uint32_t)timeout_ms)
        {
            return 0U;
        }
    }
}

void BtUart_Flush(void)
{
    s_rxTail = s_rxHead;
}

uint32_t BtUart_RxTotal(void)
{
    return s_rxTotal;
}

uint32_t BtUart_RxDropped(void)
{
    return s_rxDropped;
}

uint32_t BtUart_TxTotal(void)
{
    return s_txTotal;
}

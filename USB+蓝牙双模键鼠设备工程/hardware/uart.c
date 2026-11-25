#include <uart.h>
// 提供可变参数支持，用于 UART1_Printf 函数
#include <stdarg.h>
// 提供 vsprintf 函数，用于格式化字符串
#include <stdio.h>
#include <usb_cdc.h>
// 注意：本文件不包含 <led.h>。
// BSRR/BRR 争抢同一组引脚，因此不再在串口接收中断中直接点灯。
// 若需要「收到数据」的视觉指示，请在主循环里改用
// Tim2_SetRedBrightness() 控制红灯亮度。

// 串口接收缓冲区，存放中断中接收到的数据
volatile uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
// 接收计数器，记录当前缓冲区中已接收的字节数
volatile uint16_t uart_rx_cnt = 0;
// 接收标志位，1 表示收到新数据，主程序读取后清零
volatile uint8_t uart_rx_flag = 0;

// 串口1初始化函数
// 使用 USART1，TX 接 PA9，RX 接 PA10
// 采用 GPIO 标准库配置，并启用 NVIC 接收中断
void UART1_Init(uint32_t baudrate)
{
    // 全局统一为 3 bit 抢占优先级、1 bit 子优先级
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    // 定义 GPIO 初始化结构体，用于配置 PA9 和 PA10
    GPIO_InitTypeDef GPIO_InitStructure;
    // 定义 USART 初始化结构体，用于配置波特率、数据位、停止位等
    USART_InitTypeDef USART_InitStructure;
    // 定义 NVIC 初始化结构体，用于配置中断优先级
    NVIC_InitTypeDef NVIC_InitStructure;

    // 第1步：开启相关外设时钟
    // USART1 挂在 APB2 总线上，需要开启 APB2 时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    // PA9 和 PA10 属于 GPIOA，需要开启 GPIOA 时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    // 使用 NVIC 中断需要开启 AFIO 时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    // 第2步：配置 PA9 为复用推挽输出
    // PA9 作为 USART1_TX，需要复用功能输出
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;          // 选择 Pin9
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;  // 输出速度 50MHz
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;    // 复用推挽输出模式
    GPIO_Init(GPIOA, &GPIO_InitStructure);             // 初始化 GPIOA9

    // 第3步：配置 PA10 为上拉输入
    // PA10 作为 USART1_RX，需要输入模式
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;         // 选择 Pin10
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;      // 上拉输入模式
    GPIO_Init(GPIOA, &GPIO_InitStructure);             // 初始化 GPIOA10

    // 第4步：配置 USART1 通信参数
    USART_InitStructure.USART_BaudRate = baudrate;                       // 设置波特率
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;          // 数据位 8 位
    USART_InitStructure.USART_StopBits = USART_StopBits_1;               // 停止位 1 位
    USART_InitStructure.USART_Parity = USART_Parity_No;                  // 无校验位
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None; // 无硬件流控
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;      // 同时使能接收和发送
    USART_Init(USART1, &USART_InitStructure);                            // 应用配置到 USART1

    // 第5步：配置 NVIC，使能 USART1 中断
    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;              // 选择 USART1 中断通道
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3;      // 抢占优先级 3
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;             // 子优先级 1
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;                // 使能该中断通道
    NVIC_Init(&NVIC_InitStructure);                                // 初始化 NVIC

    // 第6步：使能 USART1 接收中断（RXNE：接收寄存器非空中断）
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    // 第7步：使能 USART1 外设，开始工作
    USART_Cmd(USART1, ENABLE);
}

// 串口1发送一个字节
// 参数 byte：要发送的 8 位数据
void UART1_SendByte(uint8_t byte)
{
    // 等待发送数据寄存器空（TXE=1 表示可以发送下一个字节）
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    // 把数据写入 USART1 数据寄存器，硬件自动发送
    USART_SendData(USART1, byte);
}

// 串口1发送一个字符串
// 参数 str：指向要发送的字符串首地址
void UART1_SendString(char *str)
{
    char *p = str;

    // 循环直到遇到字符串结束符 '\0'
    while (*p)
    {
        // 发送当前字符，然后指针后移一位
        UART1_SendByte(*p++);
    }

#if ENABLE_USB_CDC
    // 之所以放在这里而不是每个调用点：全工程几十处打印都走这个函数，
    // 在这里做镜像，一处改动就让所有日志都能从 USB 看到。
    UsbCdc_SendString(str);
#endif
}

// 串口1接收一个字节（从中断缓冲区读取）
// 参数 byte：用于保存读取到的数据
// 返回值：0 表示没有新数据，1 表示成功读取到一个字节
uint8_t UART1_ReceiveByte(uint8_t *byte)
{
    // 判断是否有新数据到达
    if (uart_rx_flag)
    {
        // 读取缓冲区中的第一个字节
        *byte = uart_rx_buf[0];
        // 清除接收标志，表示数据已被读取
        uart_rx_flag = 0;
        // 清空接收计数器，准备接收下一批数据
        uart_rx_cnt = 0;
        return 1;
    }
    // 没有新数据，返回 0
    return 0;
}

// 串口1格式化输出，类似 printf
// 参数 fmt：格式化字符串；...：可变参数列表
void UART1_Printf(char *fmt, ...)
{
    // 临时缓冲区，用于存放格式化后的字符串
    char buf[128];
    // 定义可变参数列表
    va_list ap;
    // 初始化可变参数
    va_start(ap, fmt);
    // 把格式化内容写入缓冲区
    //   就会直接踩栈，之后所有打印都会变成内存里的随机内容（乱码），
    //   严重时还会随机死机。这里用 sizeof(buf) 把上限交给库函数。
    vsnprintf(buf, sizeof(buf), fmt, ap);
    // 结束可变参数处理
    va_end(ap);
    // 通过串口发送格式化后的字符串
    UART1_SendString(buf);
}

// USART1 中断服务函数
// 当 USART1 接收到数据时，硬件自动调用此函数
void USART1_IRQHandler(void)
{
    // 定义变量保存接收到的数据
    uint8_t data;

    // 判断是否是接收中断（RXNE 标志置位）
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        // 读取接收数据寄存器，读取后 RXNE 标志会自动清零
        data = USART_ReceiveData(USART1);

        // 如果缓冲区未满，把数据存入缓冲区
        if (uart_rx_cnt < UART_RX_BUF_SIZE - 1)
        {
            // 把接收到的字节存入缓冲区当前位置，然后计数器加 1
            uart_rx_buf[uart_rx_cnt++] = data;
        }

        // 设置接收标志，通知主程序有新数据
        uart_rx_flag = 1;

        // 此处原本调用 LED_On(led1) 点亮 PC7 红灯作为接收指示。
        // 视觉指示请改由主循环调用 Tim2_SetRedBrightness() 实现。
    }
}

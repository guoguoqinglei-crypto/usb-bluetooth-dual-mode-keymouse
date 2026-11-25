#ifndef _UART_H
#define _UART_H

// 包含标准整数类型定义，例如 uint8_t、uint16_t
#include <stdint.h>
// 包含 STM32F10x 标准库头文件，提供寄存器和外设操作函数
#include <stm32f10x.h>

// 串口接收缓冲区大小，最多可缓存 64 个字节
#define UART_RX_BUF_SIZE 64

// 串口接收缓冲区，volatile 表示该变量可能在中断中随时改变
extern volatile uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
// 当前接收到的字节数
extern volatile uint16_t uart_rx_cnt;
// 串口接收标志，1 表示收到新数据，0 表示未收到或已读取
extern volatile uint8_t uart_rx_flag;

// 串口1硬件连接说明：
// PA9  - USART1_TX（发送引脚）
// PA10 - USART1_RX（接收引脚）

// 串口1初始化函数，参数 baudrate 为波特率，例如 115200
void UART1_Init(uint32_t baudrate);

// 串口1发送一个字节，参数 byte 为要发送的数据
void UART1_SendByte(uint8_t byte);

// 串口1发送一个字符串，参数 str 为字符串首地址
void UART1_SendString(char *str);

// 串口1接收一个字节（从中断缓冲区读取）
// 参数 byte：用于保存读取到的数据
// 返回值：0 表示没有新数据，1 表示成功读取到一个字节
uint8_t UART1_ReceiveByte(uint8_t *byte);

// 串口1格式化输出，类似 printf，可用于发送数字、字符串等
void UART1_Printf(char *fmt, ...);

#endif

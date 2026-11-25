#ifndef __BT_UART_H
#define __BT_UART_H

#include <stdint.h>

/* 初始化 USART2。parity 取 USART_Parity_No / USART_Parity_Even，
 * 注意偶校验时必须用 9 位字长（8 数据位 + 1 校验位）。 */
void BtUart_Init(uint32_t baud, uint16_t parity);

void BtUart_SetBaud(uint32_t baud, uint16_t parity);

/* 阻塞发送一整帧 */
void BtUart_Send(const uint8_t *data, uint16_t len);

/* 从环形缓冲取一个字节，超时返回 0 */
uint8_t BtUart_RxByte(uint8_t *out, uint16_t timeout_ms);

/* 当前缓冲区里有多少字节可读 */
uint16_t BtUart_Available(void);

/* 取一个字节（不等待），没有返回 0 */
uint8_t BtUart_TryRead(uint8_t *out);

/* 清空缓冲区 */
void BtUart_Flush(void);

/* 累计收到的字节数 / 因缓冲满或溢出丢掉的字节数 / 累计发出的字节数 */
uint32_t BtUart_RxTotal(void);
uint32_t BtUart_RxDropped(void);
uint32_t BtUart_TxTotal(void);

#endif

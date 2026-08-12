#ifndef _INC_UART_
#define _INC_UART_
#include <type.h>
#include <spinlock.h>
#include <memlayout.h>

#define GET_UART_BASE ((uint64_t)(MMIO_UART_OFFEST))
#define R_UART_REG(ofs) ((volatile uint32_t *)(GET_VIR_BASE + ofs))

// 寄存器偏移
#define UART_RXTX_OFFSET 0x00
#define UART_TXFIFO_OFFSET 0x04
#define UART_RXFIFO_OFFSET 0x08
#define UART_IE_OFFSET 0x0C
#define UART_IP_OFFSET 0x10
#define UART_DIV_OFFSET 0x14

#define UART_TXFIFO_FULL (1 << 31)
#define UART_TXFIFO_EMPTY (1 << 30)
#define UART_RXFIFO_EMPTY (1 << 31)

// uart驱动
void uart_putchar(char c);
char uart_getchar(void);
void uart_putstr(const char *s);
void uart_puthex(uint64_t val);
int uart_available(void);
void init_uart(void);
int uart_open(void *priv, int flags);
int uart_read(void *priv, void *buf, uint64_t count, uint64_t *out_len);
int uart_write(void *priv, const void *buf, uint64_t count, uint64_t *out_len);
int uart_close(void *priv);
extern struct char_device_ops uart_ops;
#endif
#include <uart.h>
#include <char_dev.h>
#include <lib.h>
#include <printk.h>
#include <sbi.h>

static spinlock_t uart_lock = {0};
static void do_screen_echo(char c, int internal_i);
struct char_device_ops uart_ops = {
    .open = uart_open,
    .read = uart_read,
    .write = uart_write,
    .close = uart_close};

void init_uart(void)
{
        init_spinlock(&uart_lock);

        // *((volatile uint32_t *)(MMIO_UART_OFFEST + UART_DIV_OFFSET)) = 26;

        // 注册字符设备
        vfs_register_chardev("ttyS0", &uart_ops, NULL);
}

int uart_open(void *priv, int flags)
{
        return 0;
}

int uart_read(void *priv, void *buf, uint64_t count, uint64_t *out_len)
{
        uint8_t *p = (uint8_t *)buf;

        // 行模式（cooked mode）：
        // - 遇到回车/换行即结束，把 \r 统一转为 \n
        // - 读满 count 字节也结束
        // - 退格删除前一个字符（缓冲区和屏幕同时回退）
        // - 返回实际读到的字节数（含末尾的 \n）
        // 必须用 while 而非 for(i;i++)：
        // 退格做 i-- 后 continue 会触发 for 的 i++，把退格抵消，
        // 导致后续字符写到错误位置，中间留下 \0 空洞。
        uint64_t i = 0;
        while (i < count)
        {
                char c = uart_getchar();

                // 回显
                do_screen_echo(c, i);

                if (c == '\r' || c == '\n')
                {
                        p[i] = '\n';
                        i++;
                        break;
                }
                if (c == '\b' || c == 0x7f)
                {
                        if (i > 0)
                        {
                                i--;
                                p[i] = '\0';
                        }
                        // i==0 时忽略退格，不存入缓冲区
                        continue;
                }

                p[i] = c;
                i++;
        }

        *out_len = i;
        return 0;
}

int uart_write(void *priv, const void *buf, uint64_t count, uint64_t *out_len)
{
        const uint8_t *p = (const uint8_t *)buf;

        for (uint64_t i = 0; i < count; i++)
        {
                uart_putchar(p[i]);
        }

        *out_len = count;
        return 0;
}

int uart_close(void *priv)
{
        return 0;
}

void uart_putchar(char c)
{
        acquire(&uart_lock);
        sbi_putchar(c);
        release(&uart_lock);
}

char uart_getchar(void)
{
        char tmp;
        acquire(&uart_lock);
        tmp = sbi_getchar();
        release(&uart_lock);
        return tmp;
}

void uart_putstr(const char *s)
{
        while (*s)
        {
                if (*s == '\n')
                        uart_putchar('\r');
                uart_putchar(*s++);
        }
}

void uart_puthex(uint64_t val)
{
        const char hex[] = "0123456789abcdef";
        if (val == 0)
        {
                uart_putstr("0x0");
                return;
        }
        uart_putstr("0x");
        int started = 0;
        for (int i = 60; i >= 0; i -= 4)
        {
                uint8_t d = (val >> i) & 0xF;
                if (d || started)
                {
                        uart_putchar(hex[d]);
                        started = 1;
                }
        }
}

// 检查是否有数据可读 non-block
int uart_available(void)
{
        return !(*((volatile uint32_t *)(MMIO_UART_OFFEST + UART_RXFIFO_OFFSET)) & UART_RXFIFO_EMPTY);
}

static void do_screen_echo(char c, int internal_i)
{
        if (c == '\r' || c == '\n')
        {
                uart_putchar('\r');
                uart_putchar('\n');
                return;
        }
        else if (c == '\b' || c == 0x7f)
        {
                if (internal_i > 0)
                {
                        // 退格
                        uart_putchar('\b');
                        uart_putchar(' ');
                        uart_putchar('\b');
                }
        }
        else
        {
                uart_putchar(c); // 普通字符直接回显
        }
}

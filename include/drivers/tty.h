#ifndef _INC_TTY_H
#define _INC_TTY_H

#include <type.h>
#include <spinlock.h>
#include <char_dev.h>

#define TTY_LINE_MAX 512

struct tty;

/// @brief 底层终端的操作接口。
///        uart_tty 和 console_tty 各实现一套。
struct tty_ops
{
    /// 打开终端，可空。
    int (*open)(struct tty *tty, int flags);
    /// 关闭终端，可空。
    int (*close)(struct tty *tty);
    /// 输出一个字符。返回 0 成功，-1 失败。
    int (*putc)(struct tty *tty, char c);
    /// 阻塞读一个字符。返回 0 成功，-1 失败。
    int (*getc)(struct tty *tty, char *out);
    /// 非阻塞检查是否有输入。返回 1 有，0 无。
    int (*has_input)(struct tty *tty);
};

/// @brief 终端会话对象。
struct tty
{
    const char *name;          /* "uart/0" / "console/0" */
    const struct tty_ops *ops; /* 底层实现 */
    void *priv;                /* 底层设备上下文 */

    /* 行编辑缓冲 */
    char linebuf[TTY_LINE_MAX];
    int  linepos;
    int  line_ready;           /* 一整行是否就绪 */

    /* 保护 linebuf / linepos / line_ready */
    spinlock_t read_lock;

    /* 读等待通道 */
    void *read_chan;

    /* 保护输出不交叉 */
    spinlock_t output_lock;
};

/// @brief 把 tty 注册成 char_device。
///        注册名由 tty->name 决定，实际路径是 "/dev/tty/<name>"。
/// @return 成功 0，失败 -1。
int tty_register(struct tty *tty);

/// @brief 初始化 TTY 层。在内核启动阶段调用一次。
void init_tty(void);

#endif
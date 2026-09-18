#include <tty.h>
#include <char_dev.h>
#include <slab.h>
#include <lib.h>
#include <printk.h>
#include <proc.h>

/* 对 VFS 的统一 ops。所有 tty 共用这一套。 */
static int tty_cdev_open(void *priv, int flags);
static int tty_cdev_read(void *priv, void *buf, uint64_t count, uint64_t *out_len);
static int tty_cdev_write(void *priv, const void *buf, uint64_t count, uint64_t *out_len);
static int tty_cdev_close(void *priv);

static struct char_device_ops tty_cdev_ops = {
    .open  = tty_cdev_open,
    .read  = tty_cdev_read,
    .write = tty_cdev_write,
    .close = tty_cdev_close,
};

/* 从 cdev->priv 取出 struct tty * */
static struct tty *tty_of_cdev(void *priv)
{
    struct char_device *cdev = (struct char_device *)priv;
    return (struct tty *)cdev->priv;
}

int tty_register(struct tty *tty)
{
    if (tty == NULL || tty->name == NULL || tty->ops == NULL)
        return -1;

    /*
     * 注册名是 "tty/<name>"，比如 "tty/uart/0"。
     * 用户态 open("/dev/tty/uart/0") 时，
     * vfs_open 去掉 "/dev/" 后是 "tty/uart/0"，正好匹配。
     */
    char fullname[64];
    strcpy(fullname, "tty/");
    strcat(fullname, tty->name);

    /*
     * char_device 结构体由 vfs_register_chardev 内部 slab 分配，
     * 这里只传 ops 和 priv。
     * name 必须是生命周期覆盖整个内核的字符串，所以这里用静态拷贝。
     */
    static char name_storage[16][64];
    static int name_slot = 0;
    if (name_slot >= 16)
        return -1;
    strcpy(name_storage[name_slot], fullname);

    return vfs_register_chardev(name_storage[name_slot++],
                                &tty_cdev_ops,
                                tty);
}

void init_tty(void)
{
    
}

static int tty_cdev_open(void *priv, int flags)
{
    struct tty *tty = tty_of_cdev(priv);
    if (tty->ops->open)
        return tty->ops->open(tty, flags);
    return 0;
}

static int tty_cdev_close(void *priv)
{
    struct tty *tty = tty_of_cdev(priv);
    if (tty->ops->close)
        return tty->ops->close(tty);
    return 0;
}

static int tty_cdev_write(void *priv, const void *buf, uint64_t count, uint64_t *out_len)
{
    struct tty *tty = tty_of_cdev(priv);
    const char *p = (const char *)buf;
    uint64_t i;

    acquire(&tty->output_lock);
    for (i = 0; i < count; i++) {
        if (tty->ops->putc(tty, p[i]) < 0) {
            release(&tty->output_lock);
            *out_len = i;
            return -1;
        }
    }
    release(&tty->output_lock);

    *out_len = count;
    return 0;
}

static int tty_cdev_read(void *priv, void *buf, uint64_t count, uint64_t *out_len)
{
    struct tty *tty = tty_of_cdev(priv);
    char *p = (char *)buf;
    uint64_t i = 0;

    /* 行模式：攒满一行或读满 count 才返回 */
    while (i < count) {
        acquire(&tty->read_lock);

        /* 攒行：从底层取字符，做行编辑 */
        while (!tty->line_ready) {
            /*
             * 先释放 read_lock，让底层 getc 去睡在自己的等待上。
             * 但这样会丢失唤醒窗口。
             * 实际实现需要底层 getc 和 read_lock 配合，
             * 这里先用 sleep_locked 的语义。
             */
            char c;
            release(&tty->read_lock);

            if (tty->ops->getc(tty, &c) < 0)
                return -1;

            acquire(&tty->read_lock);
            if (c == '\r' || c == '\n') {
                tty->linebuf[tty->linepos++] = '\n';
                tty->line_ready = 1;
            } else if (c == '\b' || c == 0x7f) {
                if (tty->linepos > 0)
                    tty->linepos--;
            } else {
                if (tty->linepos < TTY_LINE_MAX - 1)
                    tty->linebuf[tty->linepos++] = c;
            }
            release(&tty->read_lock);

            /* 回显 */
            if (tty->ops->putc)
                tty->ops->putc(tty, c);
        }

        /* 一行就绪，取走 */
        int len = tty->linepos;
        if (len > (int)(count - i))
            len = count - i;
        memcpy(p + i, tty->linebuf, len);
        i += len;
        tty->line_ready = 0;
        tty->linepos = 0;
        release(&tty->read_lock);

        if (len > 0)
            break;
    }

    *out_len = i;
    return 0;
}
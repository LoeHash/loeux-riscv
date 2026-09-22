#include <tty.h>
#include <char_dev.h>
#include <slab.h>
#include <lib.h>
#include <printk.h>
#include <proc.h>

static int tty_cdev_open(void* priv, int flags);
static int
tty_cdev_read(void* priv, void* buf, uint64_t count, uint64_t* out_len);
static int
tty_cdev_write(void* priv, const void* buf, uint64_t count, uint64_t* out_len);
static int tty_cdev_close(void* priv);

static struct char_device_ops tty_cdev_ops = {
    .open = tty_cdev_open,
    .read = tty_cdev_read,
    .write = tty_cdev_write,
    .close = tty_cdev_close,
};

int tty_register(struct tty* tty)
{
	if (tty == NULL || tty->name == NULL || tty->ops == NULL)
		return -1;

	char fullname[64];
	strcpy(fullname, "tty/");
	strcat(fullname, tty->name);

	static char name_storage[16][64];
	static int name_slot = 0;
	if (name_slot >= 16)
		return -1;
	strcpy(name_storage[name_slot], fullname);

	return vfs_register_chardev(
	    name_storage[name_slot++], &tty_cdev_ops, tty);
}

void init_tty(void)
{
	printk("the tty driver is done!\n");
}

static int tty_cdev_open(void* priv, int flags)
{
	struct tty* tty = (struct tty*)priv;
	if (tty->ops->open)
		return tty->ops->open(tty, flags);
	return 0;
}

static int tty_cdev_close(void* priv)
{
	struct tty* tty = (struct tty*)priv;
	if (tty->ops->close)
		return tty->ops->close(tty);
	return 0;
}

static int
tty_cdev_write(void* priv, const void* buf, uint64_t count, uint64_t* out_len)
{
	struct tty* tty = (struct tty*)priv;
	const char* p = (const char*)buf;
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

	/*
	 * 这里不立即刷屏：putc 只写后端 fb 并标脏，上屏由 100Hz 时钟节拍
	 * 统一合并完成。这样程序连续输出（尤其滚屏）时，10ms 内的所有脏区
	 * 只产生一次 GPU 传输 + 窗口重绘。需要立即上屏的时刻（如 shell
	 * 打印完提示符、即将阻塞等键盘输入）由 read 路径调 flush 保证。
	 * uart 后端无 flush，字符本就即时输出，不受影响。
	 */

	*out_len = count;
	return 0;
}

/*
 * 行规则：处理一个原始字符。
 * 更新行缓冲，决定回显什么。
 * 返回 1 表示这一行已就绪，0 表示还没。
 */
static int tty_line_discipline(struct tty* tty, char c)
{
	int ready = 0;

	acquire(&tty->read_lock);

	if (c == '\r' || c == '\n') {
		tty->linebuf[tty->linepos++] = '\n';
		tty->line_ready = 1;
		ready = 1;
	} else if (c == '\b' || c == 0x7f) {
		if (tty->linepos > 0)
			tty->linepos--;
	} else {
		if (tty->linepos < TTY_LINE_MAX - 1)
			tty->linebuf[tty->linepos++] = c;
	}

	release(&tty->read_lock);

	/* 回显 */
	if (c == '\r' || c == '\n') {
		tty->ops->putc(tty, '\r');
		tty->ops->putc(tty, '\n');
	} else if (c == '\b' || c == 0x7f) {
		tty->ops->putc(tty, '\b');
		tty->ops->putc(tty, ' ');
		tty->ops->putc(tty, '\b');
	} else {
		tty->ops->putc(tty, c);
	}

	return ready;
}

static int
tty_cdev_read(void* priv, void* buf, uint64_t count, uint64_t* out_len)
{
	struct tty* tty = (struct tty*)priv;
	char* p = (char*)buf;
	uint64_t i = 0;

	while (i < count) {
		/* 如果行已就绪，直接取走 */
		acquire(&tty->read_lock);
		if (tty->line_ready) {
			int len = tty->linepos;
			if (len > (int)(count - i))
				len = count - i;
			memcpy(p + i, tty->linebuf, len);
			i += len;
			tty->line_ready = 0;
			tty->linepos = 0;
			release(&tty->read_lock);
			break;
		}
		release(&tty->read_lock);

		/*
		 * 即将阻塞等输入：先把已回显的字符刷上屏。
		 * 若底层还有输入待处理（如粘贴、快速输入）则不刷，
		 * 继续批量处理，等真正要等的时候再刷一次。
		 */
		if (tty->ops->flush &&
		    (!tty->ops->has_input || !tty->ops->has_input(tty)))
			tty->ops->flush(tty);

		/* 行未就绪，从底层拿一个原始字符 */
		char c;
		if (tty->ops->getc(tty, &c) < 0) {
			*out_len = i;
			return -1;
		}

		/* 交给行规则处理，它会回显、更新缓冲、判断是否成行 */
		tty_line_discipline(tty, c);
	}

	*out_len = i;
	return 0;
}
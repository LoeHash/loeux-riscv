#include <test.h>
#include <vfs.h>
#include <printk.h>

/*
 * 标准输入输出测试（新 VFS 接口）：
 * fd 0/1/2 由 init_vfs_std() 安装为 /dev/ttyS0，
 * 访问方式统一为 fd_get 取临时引用 -> vfs_read/vfs_write -> file_put。
 */

void test_keyboard_read(void)
{
	char buf[128];
	int64_t len;
	struct file* f;

	printk("\n========== Keyboard Input Test ==========\n");
	printk("Type something and press Enter:\n");

	// 从 stdin 读取
	f = fd_get(0);

	if (f == NULL) {
		printk("stdin not available\n");
		return;
	}

	len = vfs_read(f, buf, sizeof(buf) - 1);
	file_put(f);

	if (len > 0) {
		buf[len] = '\0';
		printk("You typed: %s\n", buf);
		printk("Length: %ld bytes\n", len);
	} else {
		printk("Read failed or no input\n");
	}

	printk("========== Test Complete ==========\n");
}

void test_keyboard_echo(void)
{
	char c;
	char buf[128];
	int i = 0;
	struct file* f;

	printk("\n========== Keyboard Echo Test ==========\n");
	printk("Type characters, press Enter to exit:\n");
	printk("> ");

	f = fd_get(0);

	if (f == NULL) {
		printk("stdin not available\n");
		return;
	}

	while (1) {
		// 读取一个字符
		if (vfs_read(f, &c, 1) == 1) {
			// 回车退出
			if (c == '\r' || c == '\n') {
				printk("\n");
				break;
			}

			// 退格处理
			if (c == '\b' || c == 0x7F) {
				if (i > 0) {
					i--;
					printk("\b \b");
				}
				continue;
			}

			// 存入缓冲区
			if (i < sizeof(buf) - 1) {
				buf[i++] = c;
			}

			// 回显
			printk("%c", c);
		}
	}

	file_put(f);

	buf[i] = '\0';
	printk("You entered: %s\n", buf);
	printk("========== Test Complete ==========\n");
}

void test_write_stdout(void)
{
	char* message = "Hello, this is a test message written to fd=1!\n";
	struct file* f;

	// 直接写入fd=1
	f = fd_get(1);

	if (f == NULL)
		return;

	vfs_write(f, message, strlen(message));

	// 也可以分多次写入
	vfs_write(f, "Part 1: ", 8);
	vfs_write(f, "Hello ", 6);
	vfs_write(f, "World!\n", 7);

	file_put(f);
}

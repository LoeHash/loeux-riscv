#include <test.h>
#include <vfs.h>
#include <printk.h>

void test_keyboard_read(void)
{
        char buf[128];
        int len;

        printk("\n========== Keyboard Input Test ==========\n");
        printk("Type something and press Enter:\n");

        // 从 stdin 读取
        len = vfs_read(0, buf, sizeof(buf) - 1);
        if (len > 0)
        {
                buf[len] = '\0';
                printk("You typed: %s\n", buf);
                printk("Length: %d bytes\n", len);
        }
        else
        {
                printk("Read failed or no input\n");
        }

        printk("========== Test Complete ==========\n");
}

void test_keyboard_echo(void)
{
        char c;
        char buf[128];
        int i = 0;

        printk("\n========== Keyboard Echo Test ==========\n");
        printk("Type characters, press Enter to exit:\n");
        printk("> ");

        while (1)
        {
                // 读取一个字符
                if (vfs_read(0, &c, 1) == 1)
                {
                        // 回车退出
                        if (c == '\r' || c == '\n')
                        {
                                printk("\n");
                                break;
                        }

                        // 退格处理
                        if (c == '\b' || c == 0x7F)
                        {
                                if (i > 0)
                                {
                                        i--;
                                        printk("\b \b");
                                }
                                continue;
                        }

                        // 存入缓冲区
                        if (i < sizeof(buf) - 1)
                        {
                                buf[i++] = c;
                        }

                        // 回显
                        printk("%c", c);
                }
        }

        buf[i] = '\0';
        printk("You entered: %s\n", buf);
        printk("========== Test Complete ==========\n");
}

void test_write_stdout(void)
{
        char *message = "Hello, this is a test message written to fd=1!\n";

        // 直接写入fd=1
        vfs_write(1, message, strlen(message));

        // 也可以分多次写入
        vfs_write(1, "Part 1: ", 8);
        vfs_write(1, "Hello ", 6);
        vfs_write(1, "World!\n", 7);
}
#include <ulib.h>
#include <ufile.h>
#include <ustring.h>

#define BUF_SIZE 256

int main(void)
{
    char buf[BUF_SIZE];

    while (1) {
        /* 调用 read 读取用户输入 */
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n <= 0) {
            /* EOF 或出错则退出 */
            break;
        }

        /* 去掉末尾的换行符 */
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            n--;
        }
        buf[n] = '\0';

        /* 如果是 exit 则退出 */
        if (strcmp(buf, "exit") == 0) {
            break;
        }

        /* 否则用 write 输出输入的字符 */
        write(STDOUT_FILENO, buf, n);
        write(STDOUT_FILENO, "\n", 1);
    }

    return 0;
}
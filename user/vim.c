#include <ulib.h>
#include <stdio.h>
#include <malloc.h>

#define MAX_LINES 1000
#define MAX_LINE_LEN 512
#define COLS 160 /* 1280 / 8 */
#define ROWS 50  /* 800 / 16 */

static char *lines[MAX_LINES];
static int line_lens[MAX_LINES];
static int nlines;
static int cur_row;
static int cur_col;
static int scroll_row; /* 视口顶部行号 */
static char filename[256];
static int modified;

/* 输出转义序列到 stdout */
static void emit(const char *s, int n)
{
        write(1, (void *)s, n);
}

static void emit_str(const char *s)
{
        int n = 0;
        while (s[n])
                n++;
        emit(s, n);
}

/* \033[2J 清屏 */
static void clear_screen(void)
{
        emit_str("\033[2J");
}

/* \033[row;colH 光标定位（行列从 1 开始） */
static void move_cursor(int row, int col)
{
        char buf[24];
        int n = 0;
        buf[n++] = '\033';
        buf[n++] = '[';
        /* row+1 */
        if (row + 1 >= 100)
                buf[n++] = '0' + (row + 1) / 100;
        if (row + 1 >= 10)
                buf[n++] = '0' + ((row + 1) / 10) % 10;
        buf[n++] = '0' + (row + 1) % 10;
        buf[n++] = ';';
        /* col+1 */
        if (col + 1 >= 100)
                buf[n++] = '0' + (col + 1) / 100;
        if (col + 1 >= 10)
                buf[n++] = '0' + ((col + 1) / 10) % 10;
        buf[n++] = '0' + (col + 1) % 10;
        buf[n++] = 'H';
        emit(buf, n);
}

/* 加载文件到行缓冲区 */
static void load_file(const char *path)
{
        int fd = open(path, O_RDONLY);
        if (fd < 0)
        {
                /* 文件不存在：新建空文档 */
                nlines = 1;
                lines[0] = malloc(MAX_LINE_LEN);
                lines[0][0] = '\0';
                line_lens[0] = 0;
                return;
        }

        static char content[65536];
        int content_len = 0;
        int n;
        while ((n = read(fd,
                         content + content_len,
                         sizeof(content) - content_len)) > 0)
        {
                content_len += n;
                if (content_len >= (int)sizeof(content) - 1)
                        break;
        }
        close(fd);

        nlines = 0;
        int start = 0;
        for (int i = 0; i <= content_len; i++)
        {
                if (i == content_len || content[i] == '\n')
                {
                        int len = i - start;
                        if (nlines < MAX_LINES)
                        {
                                char *line = malloc(MAX_LINE_LEN);
                                int copy = len < MAX_LINE_LEN - 1
                                               ? len
                                               : MAX_LINE_LEN - 1;
                                int j;
                                for (j = 0; j < copy; j++)
                                        line[j] = content[start + j];
                                line[j] = '\0';
                                lines[nlines] = line;
                                line_lens[nlines] = copy;
                                nlines++;
                        }
                        start = i + 1;
                }
        }
        if (nlines == 0)
        {
                nlines = 1;
                lines[0] = malloc(MAX_LINE_LEN);
                lines[0][0] = '\0';
                line_lens[0] = 0;
        }
}

/* 保存文件 */
static void save_file(void)
{
        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
                return;
        for (int i = 0; i < nlines; i++)
        {
                write(fd, lines[i], line_lens[i]);
                if (i < nlines - 1)
                        write(fd, "\n", 1);
        }
        close(fd);
        modified = 0;
}

/* 重绘整个屏幕：覆盖式（\033[H 回到 1,1 逐行覆盖，不清屏不闪）
 * 重绘前 \033[?25l 隐藏光标、绘完定位光标后 \033[?25h 再显示，
 * 避免重绘过程中每行末尾 putc 画/擦光标造成的闪烁。 */
static void redraw(void)
{
        emit_str("\033[?25l"); /* 隐藏光标 */
        emit_str("\033[H");    /* 光标到 (1,1) */
        for (int i = 0; i < ROWS; i++)
        {
                int line_idx = scroll_row + i;
                if (line_idx < nlines)
                {
                        int len = line_lens[line_idx];
                        char *line = lines[line_idx];
                        if (len > COLS - 1)
                                len = COLS - 1; /* 防止超长行换行打乱行对齐 */
                        if (line_idx == cur_row)
                        {
                                /* 光标行：光标处包亮黄底黑字色块 */
                                for (int j = 0; j < len; j++)
                                {
                                        if (j == cur_col)
                                        {
                                                emit_str("\033[30;103m");
                                                emit(&line[j], 1);
                                                emit_str("\033[0m");
                                        }
                                        else
                                        {
                                                emit(&line[j], 1);
                                        }
                                }
                                if (cur_col >= len)
                                {
                                        /* 光标在行尾之后：画一个色块空格 */
                                        emit_str("\033[30;103m \033[0m");
                                }
                        }
                        else
                        {
                                emit(line, len);
                        }
                }
                emit_str("\033[K"); /* 清除行尾 */
                if (i < ROWS - 1)
                        emit_str("\r\n");
        }
        move_cursor(cur_row - scroll_row, cur_col);
        emit_str("\033[?25h"); /* 显示光标 */
}

/* 调整视口，保证光标始终可见 */
static void adjust_scroll(void)
{
        if (cur_row < scroll_row)
                scroll_row = cur_row;
        else if (cur_row >= scroll_row + ROWS)
                scroll_row = cur_row - ROWS + 1;
}

/* 在光标处插入字符 */
static void insert_char(char c)
{
        int len = line_lens[cur_row];
        if (len >= MAX_LINE_LEN - 1)
                return;
        char *line = lines[cur_row];
        for (int i = len; i > cur_col; i--)
                line[i] = line[i - 1];
        line[cur_col] = c;
        line_lens[cur_row] = len + 1;
        cur_col++;
        modified = 1;
}

/* 删除光标前一个字符（Backspace） */
static void delete_char(void)
{
        if (cur_col > 0)
        {
                char *line = lines[cur_row];
                for (int i = cur_col - 1; i < line_lens[cur_row]; i++)
                        line[i] = line[i + 1];
                line_lens[cur_row]--;
                cur_col--;
                modified = 1;
        }
        else if (cur_row > 0)
        {
                /* 合并到上一行行尾 */
                int prev_len = line_lens[cur_row - 1];
                int cur_len = line_lens[cur_row];
                if (prev_len + cur_len >= MAX_LINE_LEN - 1)
                        return;
                char *prev = lines[cur_row - 1];
                for (int i = 0; i < cur_len; i++)
                        prev[prev_len + i] = lines[cur_row][i];
                line_lens[cur_row - 1] = prev_len + cur_len;
                free(lines[cur_row]);
                for (int i = cur_row; i < nlines - 1; i++)
                {
                        lines[i] = lines[i + 1];
                        line_lens[i] = line_lens[i + 1];
                }
                nlines--;
                cur_row--;
                cur_col = prev_len;
                modified = 1;
        }
}

/* 在光标处换行 */
static void insert_newline(void)
{
        int len = line_lens[cur_row];
        char *line = lines[cur_row];
        int rest_len = len - cur_col;
        char *new_line = malloc(MAX_LINE_LEN);
        int copy = rest_len < MAX_LINE_LEN - 1 ? rest_len : MAX_LINE_LEN - 1;
        for (int i = 0; i < copy; i++)
                new_line[i] = line[cur_col + i];
        new_line[copy] = '\0';

        line_lens[cur_row] = cur_col;
        line[cur_col] = '\0';

        for (int i = nlines; i > cur_row + 1; i--)
        {
                lines[i] = lines[i - 1];
                line_lens[i] = line_lens[i - 1];
        }
        lines[cur_row + 1] = new_line;
        line_lens[cur_row + 1] = rest_len;
        nlines++;
        cur_row++;
        cur_col = 0;
        modified = 1;
}

static void move_up(void)
{
        if (cur_row > 0)
        {
                cur_row--;
                if (cur_col > line_lens[cur_row])
                        cur_col = line_lens[cur_row];
        }
        adjust_scroll();
}

static void move_down(void)
{
        if (cur_row < nlines - 1)
        {
                cur_row++;
                if (cur_col > line_lens[cur_row])
                        cur_col = line_lens[cur_row];
        }
        adjust_scroll();
}

static void move_left(void)
{
        if (cur_col > 0)
        {
                cur_col--;
        }
        else if (cur_row > 0)
        {
                cur_row--;
                cur_col = line_lens[cur_row];
        }
        adjust_scroll();
}

static void move_right(void)
{
        if (cur_col < line_lens[cur_row])
        {
                cur_col++;
        }
        else if (cur_row < nlines - 1)
        {
                cur_row++;
                cur_col = 0;
        }
        adjust_scroll();
}

static void page_up(void)
{
        cur_row -= ROWS;
        if (cur_row < 0)
                cur_row = 0;
        if (cur_col > line_lens[cur_row])
                cur_col = line_lens[cur_row];
        adjust_scroll();
}

static void page_down(void)
{
        cur_row += ROWS;
        if (cur_row >= nlines)
                cur_row = nlines - 1;
        if (cur_col > line_lens[cur_row])
                cur_col = line_lens[cur_row];
        adjust_scroll();
}

int main(int argc, char *argv[])
{
        if (argc < 2)
        {
                printf("usage: vim <file>\n");
                exit(1);
        }

        strcpy(filename, argv[1]);
        load_file(filename);
        cur_row = 0;
        cur_col = 0;
        scroll_row = 0;
        modified = 0;

        clear_screen();
        redraw();

        char c;
        while (read(0, &c, 1) == 1)
        {
                if (c == '\033')
                {
                        /* 转义序列：方向键 \033[A/B/C/D，翻页 \033[5~/6~ */
                        char s1, s2;
                        if (read(0, &s1, 1) != 1)
                                continue;
                        if (s1 != '[')
                                continue;
                        if (read(0, &s2, 1) != 1)
                                continue;
                        switch (s2)
                        {
                        case 'A':
                                move_up();
                                break;
                        case 'B':
                                move_down();
                                break;
                        case 'C':
                                move_right();
                                break;
                        case 'D':
                                move_left();
                                break;
                        case '5':
                        case '6':
                        {
                                /* Page Up: \033[5~  Page Down: \033[6~ */
                                char s3;
                                if (read(0, &s3, 1) != 1)
                                        break;
                                if (s3 == '~')
                                {
                                        if (s2 == '5')
                                                page_up();
                                        else
                                                page_down();
                                }
                                break;
                        }
                        }
                }
                else if (c == 0x11)
                {
                        /* Ctrl+Q 退出 */
                        break;
                }
                else if (c == 0x13)
                {
                        /* Ctrl+S 保存 */
                        save_file();
                }
                else if (c == '\r' || c == '\n')
                {
                        insert_newline();
                        adjust_scroll();
                }
                else if (c == '\b' || c == 0x7f)
                {
                        delete_char();
                        adjust_scroll();
                }
                else if (c >= 0x20 && c < 0x7f)
                {
                        insert_char(c);
                        adjust_scroll();
                }
                /* 光标色块需随光标移动重绘，统一走 redraw */
                redraw();
        }

        clear_screen();
        return 0;
}

/* The Lsh Shell
 *      check link: https://github.com/brenns10/lsh
 *      respect!
 */

#include <ulib.h>
#include <stdio.h>
#include <malloc.h>

#define LSH_MAX_LINE 1024 // 命令行最大长度
#define LSH_MAX_ARGS 64   // 最大参数个数
#define LSH_HIST_MAX 64   // 历史命令条数
#define LSH_COMP_MAX 256  // 补全候选上限

static void write_str(const char *s);
static void put_raw(char c);

int lsh_cd(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);
int lsh_clear(char **args);

char *builtin_str[] = {"cd", "help", "exit", "clear"};
int (*builtin_func[])(char **) = {&lsh_cd, &lsh_help, &lsh_exit, &lsh_clear};

int lsh_num_builtins()
{
        return sizeof(builtin_str) / sizeof(char *);
}

// ========== 内建命令实现 ==========
int lsh_cd(char **args)
{
        if (args[1] == NULL)
        {
                printf("lsh: expected argument to \"cd\"\n");
        }
        else
        {
                if (chdir(args[1]) != 0)
                {
                        printf("lsh: cd failed\n");
                }
        }
        return 1;
}

int lsh_help(char **args)
{
        int i;
        printf("Stephen Brennan's LSH\n");
        printf("Type program names and arguments, and hit enter.\n");
        printf("The following are built in:\n");
        for (i = 0; i < lsh_num_builtins(); i++)
        {
                printf("  %s\n", builtin_str[i]);
        }
        printf("Use the man command for information on other programs.\n");
        return 1;
}

int lsh_exit(char **args)
{
        return 0;
}

int lsh_clear(char **args)
{
        write_str("\033[2J\033[H");
        return 1;
}

int lsh_launch(char **args)
{
        pid_t pid;
        int status;

        pid = fork();
        if (pid == 0)
        {
                // 子进程：交给 execvp 在 PATH 里搜索命令。
                if (execvp(args[0], args) == -1)
                {
                        printf("lsh: command \"%s\" not found\n", args[0]);
                }
                exit(1);
        }
        else if (pid < 0)
        {
                printf("lsh: fork failed\n");
        }
        else
        {
                waitpid(pid, &status);
        }
        return 1;
}

// ========== 命令执行 ==========
int lsh_execute(char **args)
{
        int i;

        if (args[0] == NULL)
        {
                return 1;
        }

        for (i = 0; i < lsh_num_builtins(); i++)
        {
                if (strcmp(args[0], builtin_str[i]) == 0)
                {
                        return (*builtin_func[i])(args);
                }
        }

        return lsh_launch(args);
}

// ========== 终端控制辅助 ==========
#define CSI_EL "\033[K" /* 清除光标到行尾 */

static void put_raw(char c)
{
        write(1, &c, 1);
}

static void write_str(const char *s)
{
        if (s)
                write(1, (void *)s, strlen(s));
}

/* 提示符（可按需扩展） */
static const char *lsh_prompt(void)
{
        return "# ";
}

// ========== 行编辑器状态 ==========
static char line_buf[LSH_MAX_LINE];
static int line_len;    /* 已输入字符数 */
static int line_cursor; /* 光标在 buf 中的位置 */

/* 历史：环形缓冲。hist_count 为已存条数（不超过 LSH_HIST_MAX），
 * hist_head 指向下一个写入槽位。浏览时 hist_idx 指向当前展示的历史，
 * -1 表示当前正在输入的新行。 */
static char history[LSH_HIST_MAX][LSH_MAX_LINE];
static int hist_count = 0;
static int hist_head = 0;
static int hist_idx = -1;
static char hist_saved[LSH_MAX_LINE]; /* 浏览历史时保存当前未提交行 */

/* 重绘当前行。终端不支持 \033[C/D 光标移动，
 * 故用 \r 回到行首 + 重印 + \033[K 清尾 + 再 \r 到光标列。 */
static void lsh_redraw(int full)
{
        write_str("\r");
        write_str(lsh_prompt());
        write(1, line_buf, line_len);
        if (full)
                write_str(CSI_EL);
        write_str("\r");
        write_str(lsh_prompt());
        write(1, line_buf, line_cursor);
}

/* 在光标处插入字符 */
static void lsh_insert(char c)
{
        if (line_len >= LSH_MAX_LINE - 1)
                return;
        memmove(line_buf + line_cursor + 1, line_buf + line_cursor,
                line_len - line_cursor);
        line_buf[line_cursor] = c;
        line_cursor++;
        line_len++;
        lsh_redraw(1);
}

/* 删除光标前一个字符 */
static void lsh_backspace(void)
{
        if (line_cursor == 0)
                return;
        memmove(line_buf + line_cursor - 1, line_buf + line_cursor,
                line_len - line_cursor);
        line_cursor--;
        line_len--;
        lsh_redraw(1);
}

/* 删除光标处字符（Delete） */
static void lsh_delete(void)
{
        if (line_cursor >= line_len)
                return;
        memmove(line_buf + line_cursor, line_buf + line_cursor + 1,
                line_len - line_cursor - 1);
        line_len--;
        lsh_redraw(1);
}

static void lsh_cursor_left(void)
{
        if (line_cursor > 0)
        {
                line_cursor--;
                lsh_redraw(0);
        }
}

static void lsh_cursor_right(void)
{
        if (line_cursor < line_len)
        {
                line_cursor++;
                lsh_redraw(0);
        }
}

static void lsh_cursor_home(void)
{
        line_cursor = 0;
        lsh_redraw(0);
}

static void lsh_cursor_end(void)
{
        line_cursor = line_len;
        lsh_redraw(0);
}

/* 清空当前行内容 */
static void lsh_clear_line(void)
{
        line_len = 0;
        line_cursor = 0;
        lsh_redraw(1);
}

/* 用给定字符串替换当前行内容 */
static void lsh_set_line(const char *s)
{
        int n = strlen(s);
        if (n >= LSH_MAX_LINE)
                n = LSH_MAX_LINE - 1;
        memcpy(line_buf, s, n);
        line_buf[n] = '\0';
        line_len = n;
        line_cursor = n;
        lsh_redraw(1);
}

// ========== 历史命令 ==========
static void hist_add(const char *line)
{
        if (line[0] == '\0')
                return;
        /* 与上一条相同则不重复记录 */
        if (hist_count > 0)
        {
                int last = (hist_head - 1 + LSH_HIST_MAX) % LSH_HIST_MAX;
                if (strcmp(history[last], line) == 0)
                        return;
        }
        strncpy(history[hist_head], line, LSH_MAX_LINE - 1);
        history[hist_head][LSH_MAX_LINE - 1] = '\0';
        hist_head = (hist_head + 1) % LSH_HIST_MAX;
        if (hist_count < LSH_HIST_MAX)
                hist_count++;
}

/* 取相对当前 hist_idx 的上一条（更旧） */
static const char *hist_prev(void)
{
        if (hist_count == 0)
                return NULL;
        if (hist_idx == -1)
        {
                /* 从当前新行开始浏览：保存当前输入 */
                memcpy(hist_saved, line_buf, line_len);
                hist_saved[line_len] = '\0';
                hist_idx = (hist_head - 1 + LSH_HIST_MAX) % LSH_HIST_MAX;
        }
        else
        {
                int next = (hist_idx - 1 + LSH_HIST_MAX) % LSH_HIST_MAX;
                /* 环形缓冲中，最旧条目的下一个位置是 head；越过则停住 */
                if (hist_count < LSH_HIST_MAX)
                {
                        if (next < 0 || next >= hist_count)
                                return NULL;
                }
                else
                {
                        if (next == hist_head)
                                return NULL;
                }
                hist_idx = next;
        }
        return history[hist_idx];
}

/* 取相对当前 hist_idx 的下一条（更新）；回到新行时返回保存的输入 */
static const char *hist_next(void)
{
        if (hist_idx == -1)
                return NULL;
        if (hist_idx == (hist_head - 1 + LSH_HIST_MAX) % LSH_HIST_MAX &&
            hist_count > 0)
        {
                /* 已经是最新一条，再向下回到当前新行 */
                hist_idx = -1;
                return hist_saved;
        }
        hist_idx = (hist_idx + 1) % LSH_HIST_MAX;
        if (hist_count < LSH_HIST_MAX && hist_idx >= hist_count)
        {
                hist_idx = -1;
                return hist_saved;
        }
        return history[hist_idx];
}

// ========== Tab 补全 ==========
/* 候选列表 */
static char *comp_cands[LSH_COMP_MAX];
static int comp_n;

static void comp_reset(void)
{
        int i;
        for (i = 0; i < comp_n; i++)
                free(comp_cands[i]);
        comp_n = 0;
}

static void comp_add(const char *name)
{
        int i;
        if (comp_n >= LSH_COMP_MAX)
                return;
        /* 去重 */
        for (i = 0; i < comp_n; i++)
                if (strcmp(comp_cands[i], name) == 0)
                        return;
        comp_cands[comp_n] = malloc(strlen(name) + 1);
        if (comp_cands[comp_n])
        {
                strcpy(comp_cands[comp_n], name);
                comp_n++;
        }
}

/* 把 prefix 开头的候选筛出，返回筛出数量 */
static int comp_filter(const char *prefix, char **out, int max)
{
        int i, n = 0, plen = strlen(prefix);
        for (i = 0; i < comp_n && n < max; i++)
        {
                if (plen == 0 || strncmp(comp_cands[i], prefix, plen) == 0)
                {
                        if (out)
                                out[n] = comp_cands[i];
                        n++;
                }
        }
        return n;
}

/* 收集所有可执行命令（内建 + PATH 目录里的文件） */
static void collect_commands(void)
{
        int i;
        const char *path;
        const char *p;

        comp_reset();

        /* 内建命令 */
        for (i = 0; i < lsh_num_builtins(); i++)
                comp_add(builtin_str[i]);

        /* PATH 各目录中的文件 */
        path = get_path();
        p = path;
        while (*p)
        {
                const char *seg = p;
                const char *sep = p;
                int dirlen;
                while (*sep && *sep != ':')
                        sep++;
                dirlen = (int)(sep - seg);

                if (dirlen > 0)
                {
                        char dir[128];
                        int fd;
                        if (dirlen >= (int)sizeof(dir))
                                dirlen = sizeof(dir) - 1;
                        memcpy(dir, seg, dirlen);
                        dir[dirlen] = '\0';

                        fd = open(dir, O_READ);
                        if (fd >= 0)
                        {
                                struct dirent entries[8];
                                int n;
                                while ((n = getdents(fd, entries,
                                                      sizeof(entries))) > 0)
                                {
                                        int cnt = n / (int)sizeof(struct dirent);
                                        int j;
                                        for (j = 0; j < cnt; j++)
                                        {
                                                const char *nm =
                                                    entries[j].d_name;
                                                if (nm[0] == '.')
                                                        continue;
                                                comp_add(nm);
                                        }
                                }
                                close(fd);
                        }
                }

                if (*sep == '\0')
                        break;
                p = sep + 1;
        }
}

/* 收集当前目录中的文件名（用于参数补全） */
static void collect_files(void)
{
        int fd;
        comp_reset();

        fd = open(".", O_READ);
        if (fd < 0)
                return;
        struct dirent entries[8];
        int n;
        while ((n = getdents(fd, entries, sizeof(entries))) > 0)
        {
                int cnt = n / (int)sizeof(struct dirent);
                int j;
                for (j = 0; j < cnt; j++)
                {
                        const char *nm = entries[j].d_name;
                        if (nm[0] == '.')
                                continue;
                        comp_add(nm);
                }
        }
        close(fd);
}

/* 求候选列表的最长公共前缀长度 */
static int common_prefix_len(char **cands, int n)
{
        int i, j;
        int maxlen = strlen(cands[0]);
        for (i = 1; i < n; i++)
        {
                int l = strlen(cands[i]);
                if (l < maxlen)
                        maxlen = l;
        }
        for (j = 0; j < maxlen; j++)
        {
                char c = cands[0][j];
                for (i = 1; i < n; i++)
                        if (cands[i][j] != c)
                                return j;
        }
        return maxlen;
}

/* 找光标所在单词的起始位置（向前扫描到空白） */
static int word_start(void)
{
        int i = line_cursor - 1;
        while (i >= 0 && line_buf[i] != ' ' && line_buf[i] != '\t')
                i--;
        return i + 1;
}

/* 判断光标是否处于第一个单词（命令名）位置 */
static int is_first_word(void)
{
        int i;
        for (i = 0; i < line_cursor; i++)
                if (line_buf[i] == ' ' || line_buf[i] == '\t')
                        return 0;
        return 1;
}

static void lsh_complete(void)
{
        int ws = word_start();
        int wlen = line_cursor - ws;
        char prefix[LSH_MAX_LINE];
        char *matched[LSH_COMP_MAX];
        int n, i;
        int first = is_first_word();

        if (wlen >= (int)sizeof(prefix))
                wlen = sizeof(prefix) - 1;
        memcpy(prefix, line_buf + ws, wlen);
        prefix[wlen] = '\0';

        if (first)
                collect_commands();
        else
                collect_files();

        n = comp_filter(prefix, matched, LSH_COMP_MAX);

        if (n == 0)
        {
                /* 无匹配：响铃（暂不支持，直接返回） */
                comp_reset();
                return;
        }

        if (n == 1)
        {
                /* 唯一匹配：补全并加空格 */
                const char *full = matched[0];
                int flen = strlen(full);
                int insert_len = flen - wlen;
                if (line_len + insert_len + 1 >= LSH_MAX_LINE)
                {
                        comp_reset();
                        return;
                }
                memmove(line_buf + ws + flen, line_buf + line_cursor,
                        line_len - line_cursor);
                memcpy(line_buf + ws, full, flen);
                line_buf[ws + flen] = ' ';
                line_len += insert_len + 1;
                line_cursor = ws + flen + 1;
                lsh_redraw(1);
                comp_reset();
                return;
        }

        /* 多匹配：先插入最长公共前缀 */
        {
                int cpl = common_prefix_len(matched, n);
                if (cpl > wlen)
                {
                        int insert_len = cpl - wlen;
                        if (line_len + insert_len < LSH_MAX_LINE)
                        {
                                memmove(line_buf + ws + cpl,
                                        line_buf + line_cursor,
                                        line_len - line_cursor);
                                memcpy(line_buf + ws + wlen,
                                       matched[0] + wlen, insert_len);
                                line_len += insert_len;
                                line_cursor = ws + cpl;
                                lsh_redraw(1);
                        }
                        comp_reset();
                        return;
                }
        }

        /* 公共前缀没有新字符：换行列出所有匹配 */
        write_str("\r\n");
        for (i = 0; i < n; i++)
        {
                write_str(matched[i]);
                put_raw(' ');
        }
        write_str("\r\n");
        /* 重新显示提示符 + 当前行 */
        lsh_redraw(1);
        comp_reset();
}

// ========== 原始字符读取（处理方向键转义序列） ==========
static int read_raw(char *out)
{
        return read(0, out, 1) == 1 ? 1 : -1;
}

char *lsh_read_line(void)
{
        line_len = 0;
        line_cursor = 0;
        hist_idx = -1;
        line_buf[0] = '\0';

        /* 显示提示符 */
        write_str(lsh_prompt());

        while (1)
        {
                char c;
                if (read_raw(&c) != 1)
                {
                        /* EOF：退出 shell */
                        write_str("\r\n");
                        exit(0);
                }

                if (c == '\033')
                {
                        /* 转义序列：方向键 \033[A/B/C/D，Home \033[H，
                         * End \033[F，Delete \033[3~ */
                        char s1, s2;
                        if (read_raw(&s1) != 1)
                                continue;
                        if (s1 != '[')
                                continue;
                        if (read_raw(&s2) != 1)
                                continue;
                        switch (s2)
                        {
                        case 'A': /* Up */
                        {
                                const char *h = hist_prev();
                                if (h)
                                        lsh_set_line(h);
                                break;
                        }
                        case 'B': /* Down */
                        {
                                const char *h = hist_next();
                                if (h)
                                        lsh_set_line(h);
                                break;
                        }
                        case 'C': /* Right */
                                lsh_cursor_right();
                                break;
                        case 'D': /* Left */
                                lsh_cursor_left();
                                break;
                        case 'H': /* Home */
                                lsh_cursor_home();
                                break;
                        case 'F': /* End */
                                lsh_cursor_end();
                                break;
                        case '3': /* Delete: \033[3~ */
                        {
                                char s3;
                                if (read_raw(&s3) == 1 && s3 == '~')
                                        lsh_delete();
                                break;
                        }
                        default:
                                break;
                        }
                        continue;
                }

                if (c == '\r' || c == '\n')
                {
                        /* 回车提交 */
                        line_buf[line_len] = '\0';
                        write_str("\r\n");
                        hist_add(line_buf);
                        return line_buf;
                }

                if (c == '\t')
                {
                        lsh_complete();
                        continue;
                }

                if (c == 0x7f || c == '\b')
                {
                        lsh_backspace();
                        continue;
                }

                if (c == 0x0c)
                {
                        write_str("\033[2J\033[H");
                        lsh_redraw(1);
                        continue;
                }

                if (c == 0x15)
                {
                        lsh_clear_line();
                        continue;
                }

                if (c == 0x01) /* Ctrl+A: Home */
                {
                        lsh_cursor_home();
                        continue;
                }

                if (c == 0x05) /* Ctrl+E: End */
                {
                        lsh_cursor_end();
                        continue;
                }

                if (c >= 0x20 && c < 0x7f)
                {
                        lsh_insert(c);
                        continue;
                }

                /* 其他控制字符忽略 */
        }
}

char **lsh_split_line(char *line)
{
        static char *args[LSH_MAX_ARGS];
        int position = 0;
        char *token;

        token = strtok(line, " \t\r\n\a");
        while (token != NULL && position < LSH_MAX_ARGS - 1)
        {
                args[position++] = token;
                token = strtok(NULL, " \t\r\n\a");
        }
        args[position] = NULL;
        return args;
}

void lsh_loop(void)
{
        char *line;
        char **args;
        int status;

        do
        {
                line = lsh_read_line();
                args = lsh_split_line(line);
                status = lsh_execute(args);
        } while (status);
}

int main(int argc, char **argv)
{
        lsh_loop();
        return 0;
}

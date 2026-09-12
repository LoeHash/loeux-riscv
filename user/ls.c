#include <ulib.h>

/*
 * ls：getdents(15) 列目录项 + fstat(14) 取类型/大小。
 *
 *   ls [path ...]      不带参数列当前目录（"." 由内核解析成 cwd）
 *
 * 输出每行：权限串  大小  名字，例如
 *   -rw-r--r--       14 hello.txt
 *   drwxr-xr-x      512 tmp
 *
 * 默认不显示 '.' 开头的条目（含 FAT 盘上的 "." / ".."）。
 */

#define LS_PATH_MAX 256
#define LS_BUF_ENTRIES 8 /* 8 × sizeof(struct dirent) ≈ 2.2KB，用户栈 4 页够用 */

#include <stdio.h>

/* 由 st_mode 生成 "-rwxrwxrwx" 形式的权限串 */
static void mode_string(uint32_t mode, char out[11])
{
        static const char rwx[] = {'r', 'w', 'x'};
        static const uint32_t bits[] = {
            S_IRUSR, S_IWUSR, S_IXUSR,
            S_IRGRP, S_IWGRP, S_IXGRP,
            S_IROTH, S_IWOTH, S_IXOTH};
        int i;

        if (S_ISDIR(mode))
                out[0] = 'd';
        else if (S_ISCHR(mode))
                out[0] = 'c';
        else if (S_ISBLK(mode))
                out[0] = 'b';
        else if (S_ISLNK(mode))
                out[0] = 'l';
        else if (S_ISFIFO(mode))
                out[0] = 'p';
        else if (S_ISSOCK(mode))
                out[0] = 's';
        else
                out[0] = '-';

        for (i = 0; i < 9; i++)
                out[1 + i] = (mode & bits[i]) ? rwx[i % 3] : '-';
        out[10] = '\0';
}

static void print_entry(const char *name, const struct stat *st)
{
        char perm[11];

        mode_string(st->st_mode, perm);
        /* FAT12 卷远小于 2GB，size 转 int 显示即可 */
        printf("%s %8d %s\n", perm, (int)st->st_size, name);
}

/* 取路径最后一段（不修改原串） */
static const char *base_name(const char *path)
{
        const char *p = strrchr(path, '/');
        return p ? p + 1 : path;
}

/* 拼 "dir/name"；"." 时直接用 name，"/" 时不重复斜杠 */
static void join_path(char *dst, uint32_t cap, const char *dir, const char *name)
{
        if (strcmp(dir, ".") == 0)
        {
                strncpy(dst, name, cap - 1);
                dst[cap - 1] = '\0';
        }
        else if (strcmp(dir, "/") == 0)
        {
                dst[0] = '/';
                strncpy(dst + 1, name, cap - 2);
                dst[cap - 1] = '\0';
        }
        else
        {
                strncpy(dst, dir, cap - 1);
                dst[cap - 1] = '\0';
                if (strlen(dst) + 1 + strlen(name) + 1 <= cap)
                {
                        strcat(dst, "/");
                        strcat(dst, name);
                }
        }
}

static void ls_path(const char *path, int show_header)
{
        int fd = open(path, O_READ);
        if (fd < 0)
        {
                printf("ls: cannot open '%s'\n", path);
                return;
        }

        struct stat st;
        if (fstat(fd, &st) < 0)
        {
                printf("ls: cannot stat '%s'\n", path);
                close(fd);
                return;
        }

        /* 参数是普通文件/设备：直接打印自身 */
        if (!S_ISDIR(st.st_mode))
        {
                print_entry(base_name(path), &st);
                close(fd);
                return;
        }

        if (show_header)
                printf("%s:\n", path);

        struct dirent entries[LS_BUF_ENTRIES];
        int n;
        while ((n = getdents(fd, entries, sizeof(entries))) > 0)
        {
                int count = n / (int)sizeof(struct dirent);
                int i;

                for (i = 0; i < count; i++)
                {
                        const char *name = entries[i].d_name;

                        /* 默认不显示 "." / ".." 和隐藏条目 */
                        if (name[0] == '.')
                                continue;

                        /* 逐条 open + fstat：目录项类型走 S_ISDIR，
                           不直接读 FAT 属性字节（d_type 已在 entries 里备用） */
                        char child[LS_PATH_MAX];
                        join_path(child, sizeof(child), path, name);

                        int cfd = open(child, O_READ);
                        if (cfd < 0)
                        {
                                printf("?                   %s\n", name);
                                continue;
                        }

                        struct stat cst;
                        if (fstat(cfd, &cst) < 0)
                                printf("?                   %s\n", name);
                        else
                                print_entry(name, &cst);
                        close(cfd);
                }
        }

        if (n < 0)
                printf("ls: cannot read directory '%s'\n", path);

        close(fd);
}

int main(int argc, char **argv)
{
        int i;

        if (argc < 2)
        {
                ls_path(".", 0);
                exit(0);
        }

        for (i = 1; i < argc; i++)
                ls_path(argv[i], argc > 2);

        exit(0);
}

#include <ulib.h>
#include <stdio.h>
#include <ustring.h>
#include <ufile.h>

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#endif

#ifndef PATH_MAX
#define PATH_MAX 512
#endif

static int opt_recursive = 0;
static int opt_force = 0;
static int join_path(char *out, int outsz, const char *path, const char *name);

static int rm_file(const char *path)
{
        int ret = unlink(path);
        if (ret < 0)
        {
                if (opt_force)
                        return 0;
                printf("rm: cannot remove '%s'\n", path);
                return -1;
        }
        return 0;
}

static int rm_dir(const char *path)
{
        int ret = rmdir(path);
        if (ret < 0)
        {
                if (opt_force)
                        return 0;
                printf("rm: cannot remove '%s': Directory not empty or permission denied\n",
                       path);
                return -1;
        }
        return 0;
}

/* 判断 path 是否是目录：open + fstat。
 * 返回 1=目录, 0=非目录, -1=打开/stat 失败。 */
static int is_directory(const char *path)
{
        int fd = open(path, O_READ);
        if (fd < 0)
                return -1;

        struct stat st;
        if (fstat(fd, &st) < 0)
        {
                close(fd);
                return -1;
        }
        close(fd);
        return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* 递归删除目录 path 下所有内容，最后 rmdir(path)。 */
static int rm_tree(const char *path)
{
        int fd = open(path, O_READ);
        if (fd < 0)
        {
                if (opt_force)
                        return 0;
                printf("rm: cannot open directory '%s'\n", path);
                return -1;
        }

        int failures = 0;
        struct dirent buf[64]; /* 一次读多个 dirent */
        int n;

        while ((n = getdents(fd, buf, sizeof(buf))) > 0)
        {
                int off = 0;
                while (off < n)
                {
                        struct dirent *de = (struct dirent *)((char *)buf + off);
                        off += de->d_reclen;

                        const char *name = de->d_name;
                        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                                continue;

                        char child[PATH_MAX];
                        if (join_path(child, sizeof(child), path, name) < 0)
                        {
                                printf("rm: path too long: %s/%s\n", path, name);
                                failures++;
                                continue;
                        }

                        int is_dir;
                        if (de->d_type == DT_DIR)
                                is_dir = 1;
                        else if (de->d_type == DT_REG)
                                is_dir = 0;
                        else
                                is_dir = is_directory(child); /* DT_UNKNOWN 回退 */

                        int r;
                        if (is_dir < 0)
                                r = opt_force ? 0 : -1;
                        else if (is_dir)
                                r = rm_tree(child);
                        else
                                r = rm_file(child);

                        if (r < 0)
                                failures++;
                }
        }

        close(fd);

        if (rm_dir(path) < 0)
                failures++;

        return failures ? -1 : 0;
}

static int rm_one(const char *path)
{
        int is_dir = is_directory(path);
        if (is_dir < 0)
        {
                if (opt_force)
                        return 0;
                printf("rm: cannot remove '%s': No such file or directory\n", path);
                return -1;
        }

        if (is_dir)
        {
                if (!opt_recursive)
                {
                        printf("rm: cannot remove '%s': Is a directory\n", path);
                        return -1;
                }
                return rm_tree(path);
        }

        return rm_file(path);
}

int main(int argc, char **argv)
{
        int i;
        int failures = 0;
        int have_path = 0;

        for (i = 1; i < argc; i++)
        {
                const char *a = argv[i];
                if (a[0] == '-' && a[1] != '\0')
                {
                        const char *p = a + 1;
                        int ok = 1;
                        while (*p)
                        {
                                if (*p == 'r' || *p == 'R')
                                        opt_recursive = 1;
                                else if (*p == 'f')
                                        opt_force = 1;
                                else
                                {
                                        ok = 0;
                                        break;
                                }
                                p++;
                        }
                        if (!ok)
                        {
                                printf("rm: invalid option -- '%s'\n", a);
                                return -1;
                        }
                }
                else
                {
                        have_path = 1;
                        if (rm_one(a) < 0)
                                failures++;
                }
        }

        if (!have_path)
        {
                printf("usage: rm [-rf] <path> ...\n");
                return -1;
        }

        return failures ? -1 : 0;
}

/* 把 path + "/" + name 拼到 out，成功返回 0，溢出返回 -1 */
static int join_path(char *out, int outsz, const char *path, const char *name)
{
        int i = 0, j = 0;
        while (path[i])
        {
                if (i + 1 >= outsz)
                        return -1;
                out[i] = path[i];
                i++;
        }
        if (i + 1 >= outsz)
                return -1;
        out[i++] = '/';
        while (name[j])
        {
                if (i + 1 >= outsz)
                        return -1;
                out[i++] = name[j++];
        }
        out[i] = '\0';
        return 0;
}
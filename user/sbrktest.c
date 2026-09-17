#include <ulib.h>
#include <stdio.h>


static void fail(const char *msg)
{
        printf("FAIL: %s\n", msg);
        exit(1);
}

/* ---------- 用例 1：基本扩展 + 懒分配 ---------- */
static void test_basic(void)
{
        printf("----- [1] basic expand + lazy alloc -----\n");

        char *p = (char *)sbrk(4096);
        if (p == (void *)-1)
                fail("sbrk(4096) returned -1");
        printf("  sbrk(4096) -> old break = %p\n", p);

        p[0] = 'A';
        if (p[0] != 'A')
                fail("p[0] != 'A'");
        printf("  first write ok, p[0] = %c\n", p[0]);

        for (int i = 0; i < 4096; i++)
                p[i] = (char)(i & 0xff);
        for (int i = 0; i < 4096; i++)
                if (p[i] != (char)(i & 0xff))
                        fail("full-page r/w mismatch");
        printf("  full-page r/w ok\n");
}

/* ---------- 用例 2：跨页 / 多页扩展 ---------- */
static void test_multi_page(void)
{
        printf("----- [2] multi-page expand -----\n");

        char *q = (char *)sbrk(8192);
        if (q == (void *)-1)
                fail("sbrk(8192) returned -1");
        printf("  sbrk(8192) -> old break = %p\n", q);

        for (int i = 0; i < 8192; i += 4096) {
                q[i] = (char)('a' + i / 4096);
                if (q[i] != (char)('a' + i / 4096))
                        fail("multi-page write mismatch");
        }

        q[8191] = 'Z';
        if (q[8191] != 'Z')
                fail("q[8191] != 'Z'");
        printf("  cross-page access ok, q[8191] = %c\n", q[8191]);
}

/* ---------- 用例 3：收缩不回收，内容保留 ---------- */
static void test_shrink_no_reclaim(void)
{
        printf("----- [3] shrink keeps pages (no reclaim) -----\n");

        char *p = (char *)sbrk(4096);
        if (p == (void *)-1)
                fail("sbrk(4096) returned -1");

        p[0] = 'X';
        if (p[0] != 'X')
                fail("p[0] != 'X'");

        void *old = sbrk(-4096);
        if (old == (void *)-1)
                fail("sbrk(-4096) returned -1");
        printf("  shrink ok, old break = %p\n", old);

        /* 收缩后 p 已 >= heap_brk，不在此处访问，避免被杀 */
}

/* ---------- 用例 4：收缩后再扩展，旧页内容保留 ---------- */
static void test_reexpand_keeps_content(void)
{
        printf("----- [4] re-expand keeps old content -----\n");

        char *p = (char *)sbrk(4096);
        if (p == (void *)-1)
                fail("sbrk(4096) returned -1");

        p[0] = 'M';
        p[4095] = 'N';
        if (p[0] != 'M' || p[4095] != 'N')
                fail("initial write mismatch");

        if (sbrk(-4096) == (void *)-1)
                fail("shrink failed");

        char *r = (char *)sbrk(4096);
        if (r == (void *)-1)
                fail("re-expand failed");
        if (r != p)
                printf("  note: re-expand got %p, original %p\n", r, p);

        /* 你的实现不回收页，页表项仍在，内容应保留 */
        if (r[0] != 'M' || r[4095] != 'N')
                fail("content not preserved after re-expand");
        printf("  re-expand ok, content preserved: r[0]=%c r[4095]=%c\n",
               r[0], r[4095]);
}

/* ---------- 用例 5：边界——正好 heap_brk - 1 合法 ---------- */
static void test_boundary_last_valid(void)
{
        printf("----- [5] boundary: last valid byte -----\n");

        char *p = (char *)sbrk(4096);
        if (p == (void *)-1)
                fail("sbrk(4096) returned -1");

        /* p 是旧 break，新 break = p + 4096
         * 合法范围 [heap_start, heap_brk)，p + 4095 是最后一个合法字节
         */
        p[4095] = 'L';
        if (p[4095] != 'L')
                fail("p[4095] != 'L'");
        printf("  p[4095] (last valid) ok\n");
}

/*
 * ---------- 用例 6：边界——heap_brk 本身非法 ----------
 * 子进程访问 p[4096]（== heap_brk），应被内核 to_kill。
 * 父进程通过 wait 的 status 判断子进程是否"没正常 exit(0)"。
 */
static void test_boundary_first_invalid(void)
{
        printf("----- [6] boundary: heap_brk itself is invalid -----\n");
        printf("  (expect child killed by kernel)\n");

        int pid = fork();
        if (pid < 0) {
                printf("  fork failed, skip this case\n");
                return;
        }
        if (pid == 0) {
                /* 子进程 */
                char *p = (char *)sbrk(4096);
                if (p == (void *)-1)
                        exit(1);

                /* 越界：p + 4096 == heap_brk，应被杀 */
                p[4096] = 'X';

                /* 走到这里说明越界没被杀 —— 主动用非 0 退出标记失败 */
                printf("  child survived illegal access (BUG)\n");
                exit(2);
        }

        /* 父进程 */
        int status = -12345;
        int ret = wait(&status);
        if (ret < 0) {
                printf("  wait failed, skip\n");
                return;
        }
        if (status == 0)
                fail("child exited normally (code 0), expected kill");
        printf("  child was not a normal exit, status = %d\n", status);
}

/* ---------- 用例 7：越界访问 heap_brk 以上多字节 ---------- */
static void test_far_out_of_bounds(void)
{
        printf("----- [7] far out-of-bounds -----\n");
        printf("  (expect child killed by kernel)\n");

        int pid = fork();
        if (pid < 0) {
                printf("  fork failed, skip this case\n");
                return;
        }
        if (pid == 0) {
                char *p = (char *)sbrk(4096);
                if (p == (void *)-1)
                        exit(1);

                p[4096 * 16] = 'Y';
                printf("  child survived far OOB (BUG)\n");
                exit(2);
        }

        int status = -12345;
        int ret = wait(&status);
        if (ret < 0) {
                printf("  wait failed, skip\n");
                return;
        }
        if (status == 0)
                fail("child exited normally (code 0), expected kill");
        printf("  child was not a normal exit, status = %d\n", status);
}

/* ---------- 用例 8：heap_start 以下非法（地址 0） ---------- */
static void test_below_heap_start(void)
{
        printf("----- [8] below heap_start is invalid -----\n");
        printf("  (expect child killed by kernel)\n");

        int pid = fork();
        if (pid < 0) {
                printf("  fork failed, skip this case\n");
                return;
        }
        if (pid == 0) {
                volatile char *bad = (char *)0;
                *bad = 'Z';
                printf("  child survived null write (BUG)\n");
                exit(2);
        }

        int status = -12345;
        int ret = wait(&status);
        if (ret < 0) {
                printf("  wait failed, skip\n");
                return;
        }
        if (status == 0)
                fail("child exited normally (code 0), expected kill");
        printf("  child was not a normal exit, status = %d\n", status);
}

void test_sbrk(void)
{
        printf("===== sbrk lazy allocation test (extended) =====\n");

        test_basic();
        test_multi_page();
        test_shrink_no_reclaim();
        test_reexpand_keeps_content();
        test_boundary_last_valid();
        test_boundary_first_invalid();
        test_far_out_of_bounds();
        test_below_heap_start();

        printf("===== sbrk test passed =====\n");
}

int main(int argc, char *argv[])
{
        test_sbrk();
        return 0;
}
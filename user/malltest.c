#include <stdio.h>
#include <ustring.h>
#include <utype.h>
#include <ulib.h>
#include <malloc.h>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); return -1; } \
    else printf("ok  : %s\n", msg); \
} while (0)

/* 用地址低字节做模式填内存，方便肉眼分辨 */
static void fill(void *p, size_t n, unsigned char pat)
{
    unsigned char *c = p;
    for (size_t i = 0; i < n; i++) c[i] = pat;
}

static int verify(void *p, size_t n, unsigned char pat)
{
    unsigned char *c = p;
    for (size_t i = 0; i < n; i++)
        if (c[i] != pat) return 0;
    return 1;
}

int test_malloc(void)
{
    printf("==== malloc basic test ====\n");

    /* 1. malloc(0) 应返回 NULL（你的实现约定） */
    CHECK(malloc(0) == NULL, "malloc(0) returns NULL");

    /* 2. 单次分配 + 写读 */
    char *a = malloc(64);
    CHECK(a != NULL, "malloc(64)");
    fill(a, 64, 0xAA);
    CHECK(verify(a, 64, 0xAA), "write/read 64 bytes");

    /* 3. 对齐检查：payload 应 16 字节对齐 */
    CHECK(((uint64_t)a % 16) == 0, "malloc returns 16-aligned ptr");

    /* 4. 连续分配，互不重叠 */
    char *b = malloc(128);
    char *c = malloc(256);
    CHECK(b && c, "malloc(128), malloc(256)");
    fill(b, 128, 0xBB);
    fill(c, 256, 0xCC);
    CHECK(verify(a, 64, 0xAA), "block a intact after b,c alloc");
    CHECK(verify(b, 128, 0xBB), "block b intact");
    CHECK(verify(c, 256, 0xCC), "block c intact");

    /* 5. free 后重新分配，应复用地址 */
    free(b);
    char *b2 = malloc(128);
    CHECK(b2 == b, "malloc reuses freed block (first fit)");

    /* 6. calloc 应清零 */
    char *d = calloc(32, 4);
    CHECK(d != NULL, "calloc(32,4)");
    int zero = 1;
    for (int i = 0; i < 128; i++) if (d[i] != 0) zero = 0;
    CHECK(zero, "calloc zeroes memory");

    /* 7. realloc 扩容，旧数据保留 */
    char *e = malloc(32);
    CHECK(e != NULL, "malloc(32) for realloc");
    fill(e, 32, 0xEE);
    e = realloc(e, 256);
    CHECK(e != NULL, "realloc to 256");
    CHECK(verify(e, 32, 0xEE), "realloc preserves old data");

    /* 8. realloc 缩容，指针不变（你的实现直接返回原 ptr） */
    char *e_old = e;
    e = realloc(e, 16);
    CHECK(e == e_old, "realloc shrink returns same ptr");

    /* 9. realloc(NULL, n) == malloc(n) */
    char *f = realloc(NULL, 48);
    CHECK(f != NULL, "realloc(NULL, 48) == malloc");

    /* 10. realloc(p, 0) == free(p) + NULL */
    void *g = realloc(f, 0);
    CHECK(g == NULL, "realloc(p, 0) returns NULL");

    /* 11. free(NULL) 安全 */
    free(NULL);
    CHECK(1, "free(NULL) is safe");

    /* 12. 大量分配/释放，触发 grow_heap 和合并 */
    printf("---- stress: 1000 alloc/free ----\n");
    void *arr[100];
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 100; i++) {
            arr[i] = malloc((i % 7 + 1) * 16);
            if (!arr[i]) { printf("FAIL: stress alloc\n"); return -1; }
            fill(arr[i], (i % 7 + 1) * 16, (unsigned char)i);
        }
        for (int i = 0; i < 100; i++) {
            if (!verify(arr[i], (i % 7 + 1) * 16, (unsigned char)i)) {
                printf("FAIL: stress verify round %d i %d\n", round, i);
                return -1;
            }
        }
        for (int i = 0; i < 100; i++) free(arr[i]);
    }
    printf("ok  : 1000 alloc/free stress passed\n");

    /* 13. 交错分配，测试向后合并 */
    printf("---- coalesce test ----\n");
    char *p1 = malloc(64);
    char *p2 = malloc(64);
    char *p3 = malloc(64);
    free(p1);
    free(p2);
    free(p3);
    char *big = malloc(64 * 3);
    CHECK(big == p1, "coalesced 3 blocks into one");

    /* 清理 */
    free(a); free(c); free(b2); free(d); free(e); free(big);

    printf("==== all tests passed ====\n");
    return 0;
}

int test_coalesce_only(void) {
    char *p1 = malloc(64);
    char *p2 = malloc(64);
    char *p3 = malloc(64);
    free(p1);
    free(p2);
    free(p3);
    char *big = malloc(192);
    return (big == p1) ? 0 : -1;
}

int main(void) {
    printf("%d\n", test_coalesce_only());
    return 0;
}
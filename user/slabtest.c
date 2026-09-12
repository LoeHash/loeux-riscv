#include <stdio.h>

#define SLAB_SIZE_8_SHIFT 3
#define SLAB_SIZE_16_SHIFT 4
#define SLAB_SIZE_32_SHIFT 5
#define SLAB_SIZE_64_SHIFT 6
#define SLAB_SIZE_128_SHIFT 7
#define SLAB_SIZE_256_SHIFT 8
#define SLAB_SIZE_512_SHIFT 9
#define SLAB_SIZE_1024_SHIFT 10
#define SLAB_SIZE_2048_SHIFT 11
#define SLAB_SIZE_4096_SHIFT 12

#define SLAB_SIZE(level) (1 << SLAB_SIZE_##level##_SHIFT)

/* ===== 待测函数 ===== */
static inline uint32_t slab_round_up(uint32_t size)
{
        if (size == 0 || size > SLAB_SIZE(4096))
                return 0;

        if (size <= SLAB_SIZE(8))
                return SLAB_SIZE(8);

        size--;
        size |= size >> 1;
        size |= size >> 2;
        size |= size >> 4;
        size |= size >> 8;
        size |= size >> 16;
        size++;

        return size;
}

/* ===== 测试框架 ===== */
static int test_pass = 0;
static int test_fail = 0;

static void check(uint32_t input, uint32_t expect)
{
        uint32_t got = slab_round_up(input);
        if (got == expect)
        {
                test_pass++;
                printf("[PASS] size=%-6u -> %-6u (expect %u)\n", input, got, expect);
        }
        else
        {
                test_fail++;
                printf("[FAIL] size=%-6u -> %-6u (expect %u)\n", input, got, expect);
        }
}

int main(void)
{
        printf("===== 边界测试 =====\n");
        check(0, 0);           /* 0 返回 0 */
        check(1, 8);           /* 最小值 */
        check(8, 8);           /* 恰好等于最小 slab */
        check(9, 16);          /* 刚超过 8 */
        check(4096, 4096);     /* 最大合法值 */
        check(4097, 0);        /* 超过上限 */
        check(0xFFFFFFFFu, 0); /* 极端非法值 */

        printf("\n===== 每个 level 边界 =====\n");
        check(7, 8);
        check(8, 8);
        check(15, 16);
        check(16, 16);
        check(17, 32);
        check(31, 32);
        check(32, 32);
        check(33, 64);
        check(63, 64);
        check(64, 64);
        check(65, 128);
        check(127, 128);
        check(128, 128);
        check(129, 256);
        check(255, 256);
        check(256, 256);
        check(257, 512);
        check(511, 512);
        check(512, 512);
        check(513, 1024);
        check(1023, 1024);
        check(1024, 1024);
        check(1025, 2048);
        check(2047, 2048);
        check(2048, 2048);
        check(2049, 4096);
        check(4095, 4096);
        check(4096, 4096);

        printf("\n===== 中间值测试 =====\n");
        check(5, 8);
        check(20, 32);
        check(100, 128);
        check(1000, 1024);
        check(3000, 4096);

        printf("\n===== 与 SLAB_SIZE 宏一致性验证 =====\n");
        /* 对每个 level，验证 size=level 时返回的正是该 level */
        check(SLAB_SIZE(8), SLAB_SIZE(8));
        check(SLAB_SIZE(16), SLAB_SIZE(16));
        check(SLAB_SIZE(32), SLAB_SIZE(32));
        check(SLAB_SIZE(64), SLAB_SIZE(64));
        check(SLAB_SIZE(128), SLAB_SIZE(128));
        check(SLAB_SIZE(256), SLAB_SIZE(256));
        check(SLAB_SIZE(512), SLAB_SIZE(512));
        check(SLAB_SIZE(1024), SLAB_SIZE(1024));
        check(SLAB_SIZE(2048), SLAB_SIZE(2048));
        check(SLAB_SIZE(4096), SLAB_SIZE(4096));

        printf("\n===== 结果 =====\n");
        printf("PASS: %d, FAIL: %d\n", test_pass, test_fail);

        return test_fail == 0 ? 0 : 1;
}
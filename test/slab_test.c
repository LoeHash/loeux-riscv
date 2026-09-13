#include <slab.h>
#include <printk.h>
#include <riscv.h>
#include <panic.h>
#include <proc.h>

#define TEST_OK() printk("[SLAB TEST] PASS\n")
#define TEST_FAIL() panic_error("[SLAB TEST] FAIL\n")
extern uint64_t main_core;
static void test_slab_basic(void)
{
        printk("[SLAB TEST] basic\n");

        void *p1 = slab_alloc(8);
        void *p2 = slab_alloc(16);
        void *p3 = slab_alloc(32);
        void *p4 = slab_alloc(64);
        void *p5 = slab_alloc(128);
        void *p6 = slab_alloc(256);
        void *p7 = slab_alloc(512);
        void *p8 = slab_alloc(1024);
        void *p9 = slab_alloc(2048);

        if (!p1 || !p2 || !p3 || !p4 ||
            !p5 || !p6 || !p7 || !p8 || !p9)
                TEST_FAIL();

        slab_free(p1);
        slab_free(p2);
        slab_free(p3);
        slab_free(p4);
        slab_free(p5);
        slab_free(p6);
        slab_free(p7);
        slab_free(p8);
        slab_free(p9);

        TEST_OK();
}

static void test_slab_round_up(void)
{
        printk("[SLAB TEST] round up\n");

        void *p;

        /*
         * 1 ~ 8       -> 8
         * 9 ~ 16      -> 16
         * 17 ~ 32     -> 32
         * ...
         */

        p = slab_alloc(1);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(9);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(17);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(33);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(129);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(513);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(1025);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        TEST_OK();
}

static void test_slab_data(void)
{
        printk("[SLAB TEST] data isolation\n");

        uint64_t *a = slab_alloc(32);
        uint64_t *b = slab_alloc(32);
        uint64_t *c = slab_alloc(32);

        if (!a || !b || !c)
                TEST_FAIL();

        *a = 0x1111111111111111UL;
        *b = 0x2222222222222222UL;
        *c = 0x3333333333333333UL;

        if (*a != 0x1111111111111111UL)
                TEST_FAIL();

        if (*b != 0x2222222222222222UL)
                TEST_FAIL();

        if (*c != 0x3333333333333333UL)
                TEST_FAIL();

        slab_free(a);
        slab_free(b);
        slab_free(c);

        TEST_OK();
}

static void test_slab_reuse(void)
{
        printk("[SLAB TEST] reuse\n");

        void *p1 = slab_alloc(32);

        if (!p1)
                TEST_FAIL();

        slab_free(p1);

        void *p2 = slab_alloc(32);

        if (!p2)
                TEST_FAIL();

        /*
         * 当前 freelist 是 LIFO。
         * 所以刚 free 的对象应该优先再次被分配。
         */
        if (p1 != p2)
                TEST_FAIL();

        slab_free(p2);

        TEST_OK();
}

static void test_slab_fill_one(void)
{
        printk("[SLAB TEST] fill one slab\n");

        /*
         * 32 byte 一个 slab 可以放：
         *
         * 4096 / 32 = 128
         */
        void *objects[128];

        for (int i = 0; i < 128; i++)
        {
                objects[i] = slab_alloc(32);

                if (!objects[i])
                        TEST_FAIL();
        }

        /*
         * 128 个全部占满以后，再申请一个，
         * 应该触发新 slab 创建。
         */
        void *extra = slab_alloc(32);

        if (!extra)
                TEST_FAIL();

        for (int i = 0; i < 128; i++)
                slab_free(objects[i]);

        slab_free(extra);

        TEST_OK();
}

static void test_slab_multiple_slabs(void)
{
        printk("[SLAB TEST] multiple slabs\n");

        /*
         * 8 byte object：
         *
         * 4096 / 8 = 512
         *
         * 连续申请超过一个 slab。
         */
        void *objects[1024];

        for (int i = 0; i < 1024; i++)
        {
                objects[i] = slab_alloc(8);

                if (!objects[i])
                        TEST_FAIL();
        }

        for (int i = 0; i < 1024; i++)
                slab_free(objects[i]);

        TEST_OK();
}

static void test_slab_boundary(void)
{
        printk("[SLAB TEST] boundary\n");

        void *p;

        p = slab_alloc(8);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        p = slab_alloc(2048);
        if (!p)
                TEST_FAIL();
        slab_free(p);

        /*
         * 2049 应该无法分配。
         */
        p = slab_alloc(2049);

        if (p != NULL)
                TEST_FAIL();

        /*
         * 0 也应该失败。
         */
        p = slab_alloc(0);

        if (p != NULL)
                TEST_FAIL();

        TEST_OK();
}

void test_slab(void)
{
        printk("\n");
        printk("============================\n");
        printk("        SLAB TEST\n");
        printk("============================\n");

        test_slab_basic();
        test_slab_round_up();
        test_slab_data();
        test_slab_reuse();
        test_slab_fill_one();
        test_slab_multiple_slabs();
        test_slab_boundary();

        slab_dump();

        printk("============================\n");
        printk("      ALL SLAB TEST PASS\n");
        printk("============================\n");
}

void test_slab_race(void)
{
        static volatile uint32_t ready = 0;
        static volatile uint32_t start = 0;
        static volatile uint32_t done = 0;

        static void *result[NCPUS];

        uint64_t hart = r_tp();

        /*
         * 所有 hart 到达这里
         */
        __atomic_fetch_add(&ready, 1, __ATOMIC_SEQ_CST);

        while (__atomic_load_n(&ready, __ATOMIC_SEQ_CST) != NCPUS)
                ;

        /*
         * main hart：
         *
         * 8B slab:
         * 4096 / 8 = 512
         *
         * 把 father slab 完全占满。
         */
        if (hart == main_core)
        {
                for (int i = 0; i < 512; i++)
                {
                        if (slab_alloc(8) == NULL)
                                panic_error("[SLAB RACE] fill failed\n");
                }

                printk("[SLAB RACE] father slab full\n");

                /*
                 * 通知所有 hart：
                 */
                __atomic_store_n(&start, 1, __ATOMIC_SEQ_CST);
        }
        else
        {
                /*
                 * 等 main hart 把 father slab 填满。
                 */
                while (!__atomic_load_n(&start, __ATOMIC_SEQ_CST))
                        ;
        }

        result[hart] = slab_alloc(8);

        if (result[hart] == NULL)
                panic_error("[SLAB RACE] alloc failed\n");

        __atomic_fetch_add(&done, 1, __ATOMIC_SEQ_CST);

        /*
         * 等所有 hart 完成。
         */
        while (__atomic_load_n(&done, __ATOMIC_SEQ_CST) != NCPUS)
                ;

        /*
         * 只让 main hart 检查。
         */
        if (hart == main_core)
        {
                printk("\n[SLAB RACE] all harts finished\n");

                slab_dump();

                /*
                 * 检查每个 hart 是否拿到了不同的对象。
                 */
                for (int i = 0; i < NCPUS; i++)
                {
                        for (int j = i + 1; j < NCPUS; j++)
                        {
                                if (result[i] == result[j])
                                {
                                        panic_error(
                                            "[SLAB RACE] SAME OBJECT!\n");
                                }
                        }
                }

                printk("[SLAB RACE] all objects are different\n");

                /*
                 * 释放各 hart 得到的对象。
                 */
                for (int i = 0; i < NCPUS; i++)
                        slab_free(result[i]);

                printk("[SLAB RACE] objects freed\n");

                slab_dump();

                printk("[SLAB RACE] PASS\n");
        }

        /*
         * 防止某个 hart 提前进入 scheduler。
         */
        while (__atomic_load_n(&done, __ATOMIC_SEQ_CST) != NCPUS)
                ;
}
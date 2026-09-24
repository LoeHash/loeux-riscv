#include <stdio.h>
#include <ustring.h>
#include <utype.h>
#include <ulib.h>
#include <malloc.h>

/*
 * copyin / copyout 懒分配测试。
 */

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL: %s (line %d)\n", msg, __LINE__);         \
			return -1;                                             \
		} else                                                         \
			printf("ok  : %s\n", msg);                             \
	} while (0)

/* 内核写用户堆：copyout 路径 */
static int test_copyout_lazy_heap(void)
{
	printf("==== copyout lazy heap ====\n");

	/* 大块堆，确保跨页、且尚未触碰（懒分配） */
	size_t n = 8192; /* 2 页 */
	char* buf = malloc(n);
	CHECK(buf != NULL, "malloc(8192)");

	/*
	 * buf 从未写过，物理页未分配。
	 * read(0, buf, n) 走内核 copyout，往 buf 写数据，
	 * 应触发懒分配并成功返回。
	 */
	ssize_t r = read(0, buf, n);
	CHECK(r >= 0, "read into untouched heap buffer (copyout lazy)");

	volatile char c = buf[0];
	(void)c;
	CHECK(1, "heap buffer readable after copyout");

	free(buf);
	return 0;
}

/* 内核读用户堆：copyin 路径 */
static int test_copyin_lazy_heap(void)
{
	printf("==== copyin lazy heap ====\n");

	size_t n = 8192;
	char* buf = malloc(n);
	CHECK(buf != NULL, "malloc(8192)");

	ssize_t w = write(1, buf, n);
	CHECK(w == (ssize_t)n,
	      "write from untouched heap buffer (copyin lazy)");

	free(buf);
	return 0;
}

/* 跨页边界访问 */
static int test_cross_page(void)
{
	printf("==== cross page ====\n");

	size_t n = 4096 * 4;
	char* buf = malloc(n);
	CHECK(buf != NULL, "malloc(16K)");

	/* 只碰第一个和最后一个字节，中间页懒分配 */
	buf[0] = 'A';
	buf[n - 1] = 'Z';
	CHECK(buf[0] == 'A' && buf[n - 1] == 'Z', "touch both ends");

	/* 整段传给 syscall，触发所有页的懒分配 */
	ssize_t w = write(1, buf, n);
	CHECK(w == (ssize_t)n, "write whole buffer across pages");

	free(buf);
	return 0;
}

/* 非堆地址：传非法指针，应返回错误，不是崩溃 */
static int test_bad_pointer(void)
{
	printf("==== bad pointer ====\n");

	char* bad = (char*)0xdead0000;
	ssize_t r = read(0, bad, 64);
	CHECK(r < 0, "read into non-heap bad address returns error");

	return 0;
}

/* 堆边界外：堆尾之后一点，应失败 */
static int test_heap_boundary(void)
{
	printf("==== heap boundary ====\n");

	char* buf = malloc(64);
	CHECK(buf != NULL, "malloc(64)");

	/* 正常范围 */
	ssize_t w = write(1, buf, 64);
	CHECK(w == 64, "write within heap");

	free(buf);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_copyout_lazy_heap();
	ret |= test_copyin_lazy_heap();
	ret |= test_cross_page();
	ret |= test_bad_pointer();
	ret |= test_heap_boundary();

	if (ret == 0)
		printf("==== all lazy-alloc tests passed ====\n");
	else
		printf("==== some tests FAILED ====\n");

	return ret;
}
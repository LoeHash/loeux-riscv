#ifndef __LIB_H__
#define __LIB_H__

#include <stddef.h>
#include <stdint.h>

#define MAX(a, b) a > b ? a : b
#define MIN(a, b) a < b ? a : b

static inline char* strcpy_with_terminate(char* s, const char* t, int n)
{
	char* os;

	os = s;
	if (n <= 0)
		return os;
	while (--n > 0 && (*s++ = *t++) != 0)
		;
	*s = 0;
	return os;
}

/*
 * 内存搬运/填充按 uint64_t 宽度进行（RV64 上 LD/SD 一次 8 字节）。
 * 之前是按字节循环：清/滚一次 1280x800x4 的 framebuffer 要迭代
 * 400 万次，在 QEMU TCG 下极慢，是滚屏卡顿的重要来源。
 * 仅当源、目的按 8 字节同余对齐时走宽拷贝，否则退回逐字节
 */
extern inline __attribute__((always_inline, gnu_inline)) void*
memmove(void* dst, const void* src, uint32_t n)
{
	const uint8_t* s = (const uint8_t*)src;
	uint8_t* d = (uint8_t*)dst;

	if (n == 0 || dst == src)
		return dst;

	if (s < d && s + n > d) {
		/* 重叠且目的在后：从尾部反向拷贝 */
		const uint8_t* es = s + n;
		uint8_t* ed = d + n;

		if ((((uintptr_t)es | (uintptr_t)ed) & 7) == 0) {
			while (n >= 8) {
				es -= 8;
				ed -= 8;
				*(uint64_t*)ed = *(const uint64_t*)es;
				n -= 8;
			}
		}
		while (n-- > 0)
			*--ed = *--es;
	} else {
		/* 正向拷贝 */
		if ((((uintptr_t)s | (uintptr_t)d) & 7) == 0) {
			const uint64_t* ws = (const uint64_t*)s;
			uint64_t* wd = (uint64_t*)d;
			while (n >= 8) {
				*wd++ = *ws++;
				n -= 8;
			}
			s = (const uint8_t*)ws;
			d = (uint8_t*)wd;
		}
		while (n-- > 0)
			*d++ = *s++;
	}

	return dst;
}

/**
 * memset - 将内存区域填满指定字符 (RISC-V)
 * @ptr:  起始地址
 * @value: 填充字符（按 unsigned char 处理）
 * @num:  字节数
 */
extern inline __attribute__((always_inline, gnu_inline)) void*
memset(void* ptr, int value, size_t num)
{
	uint8_t* p = (uint8_t*)ptr;
	uint8_t val = (uint8_t)value;
	size_t i = 0;

	/* 头部补到 8 字节对齐 */
	while (i < num && (((uintptr_t)(p + i)) & 7))
		p[i++] = val;

	if (i + 8 <= num) {
		uint64_t wval = (uint64_t)val * 0x0101010101010101ULL;
		uint64_t* wp = (uint64_t*)(p + i);
		size_t cnt = (num - i) / 8;
		while (cnt--)
			*wp++ = wval;
		i = (uint8_t*)wp - p;
	}

	while (i < num)
		p[i++] = val;

	return ptr;
}

extern inline __attribute__((always_inline, gnu_inline)) int
memcmp(const void* s1, const void* s2, size_t n)
{
	const unsigned char* a = (const unsigned char*)s1;
	const unsigned char* b = (const unsigned char*)s2;

	for (; n > 0; n--, a++, b++) {
		if (*a != *b)
			return *a - *b;
	}

	return 0;
}

/**
 * memcpy - 拷贝内存区域（不允许重叠）(RISC-V)
 * @dest:  目标地址
 * @src:   源地址
 * @num:   字节数
 */
extern inline __attribute__((always_inline, gnu_inline)) void*
memcpy(void* dest, const void* src, size_t num)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t* s = (const uint8_t*)src;
	size_t i = 0;

	/* 两端都 8 字节对齐时按 uint64_t 拷贝 */
	if ((((uintptr_t)s | (uintptr_t)d) & 7) == 0) {
		const uint64_t* ws = (const uint64_t*)s;
		uint64_t* wd = (uint64_t*)d;
		while (i + 8 <= num) {
			*wd++ = *ws++;
			i += 8;
		}
		s = (const uint8_t*)ws;
		d = (uint8_t*)wd;
	}

	while (i < num) {
		*d++ = *s++;
		i++;
	}

	return dest;
}

/**
 * strlen - 返回字符串长度（不含结尾 '\0'）
 * @s: 以 '\0' 结尾的字符串
 */
static inline size_t strlen(const char* s)
{
	const char* p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}
/**
 * strcmp - 比较两个字符串
 * @s1, @s2: 以 '\0' 结尾的字符串
 * 返回：0 相等，<0 s1 < s2，>0 s1 > s2
 */
static inline int strcmp(const char* s1, const char* s2)
{
	while (*s1 && (*s1 == *s2)) {
		s1++;
		s2++;
	}
	return (unsigned char)*s1 - (unsigned char)*s2;
}
/**
 * strncmp - 比较前 n 个字符
 * @s1, @s2: 字符串
 * @n: 最多比较的字符数
 */
static inline int strncmp(const char* s1, const char* s2, size_t n)
{
	while (n-- && *s1 && (*s1 == *s2)) {
		s1++;
		s2++;
	}
	if (n == (size_t)-1) // n 递减后溢出，表示 n 初始为 0
		return 0;
	return (unsigned char)*s1 - (unsigned char)*s2;
}
/**
 * strcpy - 将 src 复制到 dest（包括 '\0'）
 * @dest: 目标缓冲区（必须足够大）
 * @src: 源字符串
 * 返回 dest
 */
static inline char* strcpy(char* dest, const char* src)
{
	char* p = dest;
	while ((*p++ = *src++))
		;
	return dest;
}
/**
 * strncpy - 复制最多 n 个字符，若 src 长度 < n 则用 '\0' 填充
 * @dest: 目标缓冲区
 * @src: 源字符串
 * @n: 最大复制字符数
 * 返回 dest
 */
static inline char* strncpy(char* dest, const char* src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i]; i++)
		dest[i] = src[i];
	for (; i < n; i++)
		dest[i] = '\0';
	return dest;
}
/**
 * strcat - 将 src 追加到 dest 末尾
 * @dest: 目标字符串（已有 '\0'）
 * @src: 源字符串
 * 返回 dest
 */
static inline char* strcat(char* dest, const char* src)
{
	char* p = dest;
	while (*p)
		p++;		// 找到 dest 结尾
	while ((*p++ = *src++)) // 复制 src
		;
	return dest;
}
/**
 * strncat - 追加最多 n 个字符，并加 '\0'
 * @dest: 目标字符串
 * @src: 源字符串
 * @n: 最大追加字符数
 * 返回 dest
 */
static inline char* strncat(char* dest, const char* src, size_t n)
{
	char* p = dest;
	size_t i;
	while (*p)
		p++;
	for (i = 0; i < n && src[i]; i++)
		p[i] = src[i];
	p[i] = '\0';
	return dest;
}
/**
 * strchr - 在字符串中查找字符第一次出现的位置
 * @s: 字符串
 * @c: 要查找的字符（会先转为 char 比较）
 * 返回：找到则返回指向该字符的指针，否则 NULL
 */
static inline char* strchr(const char* s, int c)
{
	char ch = (char)c;
	while (*s && *s != ch)
		s++;
	if (*s == ch)
		return (char*)s;
	return NULL;
}
/**
 * strrchr - 在字符串中查找字符最后一次出现的位置
 * @s: 字符串
 * @c: 要查找的字符
 * 返回：指向最后出现位置的指针，或 NULL
 */
static inline char* strrchr(const char* s, int c)
{
	char ch = (char)c;
	const char* last = NULL;
	while (*s) {
		if (*s == ch)
			last = s;
		s++;
	}
	// 注意：如果 c == '\0'，则 last 会被设置为指向结尾 '\0'
	// 这是标准行为，所以需要额外判断：
	if (ch == '\0')
		return (char*)s;
	return (char*)last;
}
/**
 * strstr - 子串查找
 * @haystack: 主字符串
 * @needle: 要查找的子串
 * 返回：指向第一次出现的位置，或 NULL
 */
static inline char* strstr(const char* haystack, const char* needle)
{
	if (!*needle)
		return (char*)haystack;
	while (*haystack) {
		const char* h = haystack;
		const char* n = needle;
		while (*h && *n && (*h == *n)) {
			h++;
			n++;
		}
		if (!*n)
			return (char*)haystack;
		haystack++;
	}
	return NULL;
}

static inline int strcasecmp(const char* s1, const char* s2)
{
	while (*s1 && *s2) {
		char c1 = *s1;
		char c2 = *s2;

		// 转大写
		if (c1 >= 'a' && c1 <= 'z')
			c1 -= 'a' - 'A';
		if (c2 >= 'a' && c2 <= 'z')
			c2 -= 'a' - 'A';

		if (c1 != c2)
			return c1 - c2;
		s1++;
		s2++;
	}
	return *s1 - *s2;
}
#endif
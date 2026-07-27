#ifndef __LIB_H__
#define __LIB_H__

#include <stddef.h>

/**
 * memset - 将内存区域填满指定字符 (RISC-V)
 * @ptr:  起始地址
 * @value: 填充字符（按 unsigned char 处理）
 * @num:  字节数
 */
static inline void *memset(void *ptr, int value, size_t num)
{
        unsigned char *p = (unsigned char *)ptr;
        unsigned char val = (unsigned char)value;

        for (size_t i = 0; i < num; i++)
                p[i] = val;

        return ptr;
}

/**
 * memcpy - 拷贝内存区域（不允许重叠）(RISC-V)
 * @dest:  目标地址
 * @src:   源地址
 * @num:   字节数
 */
static inline void *memcpy(void *dest, const void *src, size_t num)
{
        unsigned char *d = (unsigned char *)dest;
        const unsigned char *s = (const unsigned char *)src;

        for (size_t i = 0; i < num; i++)
                d[i] = s[i];

        return dest;
}

/**
 * strlen - 返回字符串长度（不含结尾 '\0'）
 * @s: 以 '\0' 结尾的字符串
 */
static inline size_t strlen(const char *s)
{
        const char *p = s;
        while (*p)
                p++;
        return (size_t)(p - s);
}
/**
 * strcmp - 比较两个字符串
 * @s1, @s2: 以 '\0' 结尾的字符串
 * 返回：0 相等，<0 s1 < s2，>0 s1 > s2
 */
static inline int strcmp(const char *s1, const char *s2)
{
        while (*s1 && (*s1 == *s2))
        {
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
static inline int strncmp(const char *s1, const char *s2, size_t n)
{
        while (n-- && *s1 && (*s1 == *s2))
        {
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
static inline char *strcpy(char *dest, const char *src)
{
        char *p = dest;
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
static inline char *strncpy(char *dest, const char *src, size_t n)
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
static inline char *strcat(char *dest, const char *src)
{
        char *p = dest;
        while (*p)
                p++;            // 找到 dest 结尾
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
static inline char *strncat(char *dest, const char *src, size_t n)
{
        char *p = dest;
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
static inline char *strchr(const char *s, int c)
{
        char ch = (char)c;
        while (*s && *s != ch)
                s++;
        if (*s == ch)
                return (char *)s;
        return NULL;
}
/**
 * strrchr - 在字符串中查找字符最后一次出现的位置
 * @s: 字符串
 * @c: 要查找的字符
 * 返回：指向最后出现位置的指针，或 NULL
 */
static inline char *strrchr(const char *s, int c)
{
        char ch = (char)c;
        const char *last = NULL;
        while (*s)
        {
                if (*s == ch)
                        last = s;
                s++;
        }
        // 注意：如果 c == '\0'，则 last 会被设置为指向结尾 '\0'
        // 这是标准行为，所以需要额外判断：
        if (ch == '\0')
                return (char *)s;
        return (char *)last;
}
/**
 * strstr - 子串查找
 * @haystack: 主字符串
 * @needle: 要查找的子串
 * 返回：指向第一次出现的位置，或 NULL
 */
static inline char *strstr(const char *haystack, const char *needle)
{
        if (!*needle)
                return (char *)haystack;
        while (*haystack)
        {
                const char *h = haystack;
                const char *n = needle;
                while (*h && *n && (*h == *n))
                {
                        h++;
                        n++;
                }
                if (!*n)
                        return (char *)haystack;
                haystack++;
        }
        return NULL;
}

#endif
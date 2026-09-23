/* Minimal memcpy/memset for GCC-generated calls in lcc */
typedef unsigned long size_t;

void *memcpy(void *dest, const void *src, size_t num)
{
	char *d = (char *)dest;
	const char *s = (const char *)src;
	while (num--)
		*d++ = *s++;
	return dest;
}

void *memset(void *ptr, int value, size_t num)
{
	char *p = (char *)ptr;
	while (num--)
		*p++ = (char)value;
	return ptr;
}

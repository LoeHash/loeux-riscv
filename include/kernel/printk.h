#ifndef _INC_PRINTK
#define _INC_PRINTK

#include <stdarg.h>
#include <type.h>

#define PRINT_BUFFER_SIZE 1024
#define FOTMAT_LEFT 1 << 0
#define FOTMAT_PLUS 1 << 1
#define FOTMAT_SPACE 1 << 2
#define FOTMAT_SPECIAL 1 << 3
#define FOTMAT_ZERO 1 << 4

#define ZEROPAD 1  /* pad with zero */
#define SIGN 2     /* unsigned/signed long */
#define PLUS 4     /* show plus */
#define SPACE 8    /* space if plus */
#define LEFT 16    /* left justified */
#define SPECIAL 32 /* 0x */
#define SMALL 64   /* use 'abcdef' instead of 'ABCDEF' */

int vsprintf(char *buf, const char *fmt, va_list args);
void printk(char *fmt, ...);

int skip_atoi(const char **s);

static char *number(char *str, long num, int base, int size, int precision, int type);

extern spinlock_t printing_lock;
#endif
#ifndef _INC_STDIO_H
#define _INC_STDIO_H
#include <stdarg.h>
#include <utype.h>
#include <ustring.h>
#include <ufile.h>
#include <umath.h>
#include <ulib.h>

#define EXIT_SUCCESS (0)
#define EXIT_FAILURE (1)
#define EOF (-1)

int scanf(const char *fmt, ...);
int getchar(void);
int printf(const char *fmt, ...);
#endif
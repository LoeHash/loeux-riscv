#ifndef _INC_STDINT_H
#define _INC_STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;

typedef short              int16_t;
typedef unsigned short     uint16_t;

typedef int                int32_t;
typedef unsigned int       uint32_t;

typedef long               int64_t;      /* 改这里 */
typedef unsigned long      uint64_t;     /* 改这里 */

typedef long               intptr_t;
typedef unsigned long      uintptr_t;

typedef long               intmax_t;
typedef unsigned long      uintmax_t;

#define INT8_MAX    127
#define INT8_MIN    (-128)
#define UINT8_MAX   255U

#define INT16_MAX   32767
#define INT16_MIN   (-32768)
#define UINT16_MAX  65535U

#define INT32_MAX   2147483647
#define INT32_MIN   (-2147483647 - 1)
#define UINT32_MAX  4294967295U

#define INT64_MAX   9223372036854775807L
#define INT64_MIN   (-9223372036854775807L - 1)
#define UINT64_MAX  18446744073709551615UL

#endif
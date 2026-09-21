#ifndef _INC_UTILS_MATH_
#define _INC_UTILS_MATH_

#define POW2_SHIFT_COUNT(x) (__builtin_ctz(x))

static inline long long max(long long a, long long b)
{
    long long ret = a;
    asm volatile(
        "bge %1, %2, 1f\n\t"
        "mv  %0, %2\n\t"
        "1:\n\t"
        : "+r"(ret)
        : "r"(a), "r"(b)
        : "memory"
    );
    return ret;
}

static inline long long min(long long a, long long b)
{
    long long ret = a;
    asm volatile(
        "ble %1, %2, 1f\n\t"   // if a <= b, keep ret
        "mv  %0, %2\n\t"
        "1:\n\t"
        : "+r"(ret)
        : "r"(a), "r"(b)
        : "memory"
    );
    return ret;
}

static inline long long abs(long long x)
{
    long long ret = x;
    asm volatile(
        "bgez %0, 1f\n\t"      // if x >= 0, done
        "neg  %0, %0\n\t"
        "1:\n\t"
        : "+r"(ret)
        :
        : "memory"
    );
    return ret;
}


#endif
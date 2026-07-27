#ifndef _INC_MEMORY_
#define _INC_MEMORY_
#define NR_BANKS_SIZE 16
typedef unsigned long phys_addr_t;

struct bank
{
        phys_addr_t base;
        phys_addr_t size;
};

struct memory_info
{
        struct bank banks[8];
        int nr_banks;
};

extern struct memory_info mem_info;

#endif
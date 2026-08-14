#ifndef _TEST_H
#define _TEST_H
#include <type.h>
#include <printk.h>
#include <virtio.h>
#include <fdt.h>
#include <vm.h>
#include <lib.h>

void test_keyboard_echo(void);
void test_keyboard_read(void);
void print_memory_info();
void test_page_alloc_free();
void print_fdt_list();
void print_fdt_header(struct fdt_header *hdr);
void vmprint(page_table pgtb);
void print_mount_table(void);
void test_fat12_operations(void);
void test_write_verify();
void dump_sector_n(struct virtio_blk_disk *disk, int n);
void test_write_stdout(void);
#endif
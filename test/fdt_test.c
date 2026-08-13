#include <fdt.h>
#include <test.h>
#include <printk.h>
void print_fdt_header(struct fdt_header *hdr)
{
        printk("========== FDT Header ==========\n");
        printk("magic:             0x%08x\n", hdr->magic);
        printk("totalsize:         0x%08x (%u bytes)\n", hdr->totalsize, hdr->totalsize);
        printk("off_dt_struct:     0x%08x (%u)\n", hdr->off_dt_struct, hdr->off_dt_struct);
        printk("off_dt_strings:    0x%08x (%u)\n", hdr->off_dt_strings, hdr->off_dt_strings);
        printk("off_mem_rsvmap:    0x%08x (%u)\n", hdr->off_mem_rsvmap, hdr->off_mem_rsvmap);
        printk("version:           0x%08x (%u)\n", hdr->version, hdr->version);
        printk("last_comp_version: 0x%08x (%u)\n", hdr->last_comp_version, hdr->last_comp_version);
        printk("boot_cpuid_phys:   0x%08x (%u)\n", hdr->boot_cpuid_phys, hdr->boot_cpuid_phys);
        printk("size_dt_strings:   0x%08x (%u bytes)\n", hdr->size_dt_strings, hdr->size_dt_strings);
        printk("size_dt_struct:    0x%08x (%u bytes)\n", hdr->size_dt_struct, hdr->size_dt_struct);
        printk("================================\n");
}
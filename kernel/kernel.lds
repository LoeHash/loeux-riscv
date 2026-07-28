OUTPUT_ARCH(riscv)
ENTRY(_entry64)

SECTIONS
{
        . = 0x80200000;

        .text : {
                *(.text._entry64) /* 定死 0x80200000 处必为入口汇编 */
                *(.text*)
        }

        .rodata : { *(.rodata*)}
        .data : { *(.data*)}

        .bss : {
                . = ALIGN(8);
                _bss_start = .;        /* 预先在链接脚本中定义 */ 
                *(.bss*)
                *(COMMON)
                . = ALIGN(8);
                _bss_end = .;
        }
        
        /* 接下来定义栈空间 */
        /* 在bss结尾处, 预留 4mb 的栈空间 */
        . += 4194304;
        _stack_top = .;
        . = ALIGN(256);
        _phy_start = .;
 
}
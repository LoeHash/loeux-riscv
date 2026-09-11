.section .text
.global _start
_start:
    call main
    li a7, 1          # sys_exit = 1
    li a0, 1212
    ecall
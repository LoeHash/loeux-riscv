.section .text
.global _start
_start:
    call main
    li a7, 56          # sys_exit = 56
    li a0, 1212
    ecall
.section .text
.global _start
_start:
    call main
    li a7, 57          # sys_exit = 57
    li a0, 1212
    ecall             
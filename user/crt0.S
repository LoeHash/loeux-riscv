.section .text
.global _start
_start:
    call main
    li a7, 57          # sys_exit = 7
    ecall             
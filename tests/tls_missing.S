/* TLS before initialization must produce a diagnostic, not a host CRT crash. */
    .text
    .globl _start
_start:
#ifdef __x86_64__
    movq %fs:8, %rax
    movl $231, %eax
    movl $99, %edi
    syscall
#else
    movl %gs:4, %eax
    movl $252, %eax
    movl $99, %ebx
    int $0x80
#endif

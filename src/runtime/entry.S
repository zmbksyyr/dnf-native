#ifdef _WIN64
    .text
    .globl nr_enter
nr_enter:
    movq %rcx, %rax
    movq %rdx, %rsp
    xorl %ebp, %ebp
    xorl %edx, %edx
    cld
    jmp *%rax
#else
    .text
    .globl _nr_enter
_nr_enter:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    movl %edx, %esp
    xorl %ebp, %ebp
    xorl %edx, %edx
    cld
    jmp *%eax
#endif

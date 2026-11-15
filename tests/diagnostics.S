.text
.globl _start
.type _start,@function
_start:
#ifdef __x86_64__
    push %rbp
    mov %rsp, %rbp
    mov $5, %r12d
#else
    push %ebp
    mov %esp, %ebp
    mov $5, %edi
#endif
again:
    mov $65535, %eax
#ifdef __x86_64__
    syscall
#else
    int $0x80
#endif
    cmp $-38, %eax
    jne bad_result
#ifdef __x86_64__
    dec %r12d
#else
    dec %edi
#endif
    jne again
    call crash_leaf
bad_result:
    ud2
.size _start,.-_start

.type crash_leaf,@function
crash_leaf:
#ifdef __x86_64__
    push %rbp
    mov %rsp, %rbp
#else
    push %ebp
    mov %esp, %ebp
#endif
    mov $0x12345678, %ebx
    xor %eax, %eax
    mov (%eax), %eax
    ret
.size crash_leaf,.-crash_leaf
.section .note.GNU-stack,"",@progbits

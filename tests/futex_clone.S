.text
.global spawn
.type spawn,@function
spawn:
#ifdef __x86_64__
    mov %rsi, %r9
    mov %rdi, %rsi
    mov %rdx, %r8
    mov %r9, %rdx
    mov %rcx, %r10
    mov $0x3d0f00, %edi
    mov $56, %eax
    syscall
    test %rax, %rax
    jz 1f
    ret
1:
    xor %ebp, %ebp
    mov (%rsp), %rdi
    call child_main
#else
    push %ebx
    push %esi
    push %edi
    mov 16(%esp), %ecx
    mov 20(%esp), %edx
    mov 24(%esp), %esi
    mov 28(%esp), %edi
    mov $0x3d0f00, %ebx
    mov $120, %eax
    int $0x80
    test %eax, %eax
    jz 1f
    pop %edi
    pop %esi
    pop %ebx
    ret
1:
    xor %ebp, %ebp
    mov (%esp), %eax
    push %eax
    call child_main
#endif
    ud2
.size spawn,.-spawn
.section .note.GNU-stack,"",@progbits

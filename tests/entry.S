    .text
    .globl _start
_start:
#ifdef __x86_64__
    testq %rdx, %rdx
    jne failed
    testq $15, %rsp
    jne failed
    cmpq $3, (%rsp)
    jne failed
    movq 16(%rsp), %rsi
    cmpb $0xe4, (%rsi)
    jne failed
    cmpq $0, 32(%rsp)
    jne failed
    cmpb $0, storage+17(%rip)
    jne failed
    movq $1, %rax
    movq $1, %rdi
    leaq message(%rip), %rsi
    movq $14, %rdx
    syscall
    cmpq $14, %rax
    jne failed
    movq $1, %rax
    movq $99, %rdi
    syscall
    cmpq $-9, %rax
    jne failed
    movq $1, %rax
    movq $1, %rdi
    movq $1, %rsi
    syscall
    cmpq $-14, %rax
    jne failed
    movdqu simd_pattern(%rip), %xmm6
    xorq %rax, %rax
    pushfq
    popq %r12
#ifdef NR_REDZONE
    movabsq $0x91a2b3c4d5e6f708, %r13
    leaq -128(%rsp), %rdi
    movq %r13, %rax
    movl $16, %ecx
    rep stosq
#endif
#ifdef NR_TLS_FAULT
    movq %fs:0, %rax
#endif
    movq $0x7fffffff, %rax
    syscall
after_syscall:
    leaq after_syscall(%rip), %r14
    cmpq %r14, %rcx
    jne failed
    cmpq %r12, %r11
    jne failed
    cmpq $-38, %rax
    jne failed
    pcmpeqb simd_pattern(%rip), %xmm6
    pmovmskb %xmm6, %eax
    cmpl $65535, %eax
    jne failed
#ifdef NR_REDZONE
    leaq -128(%rsp), %rsi
    movl $16, %ecx
check_redzone:
    cmpq %r13, (%rsi)
    jne failed
    addq $8, %rsi
    loop check_redzone
#endif
    movq $60, %rax
    movq $42, %rdi
    syscall
failed:
    movq $60, %rax
    movq $99, %rdi
    syscall
#else
    testl %edx, %edx
    jne failed
    testl $15, %esp
    jne failed
    cmpl $3, (%esp)
    jne failed
    movl 8(%esp), %esi
    cmpb $0xe4, (%esi)
    jne failed
    cmpl $0, 16(%esp)
    jne failed
    cmpb $0, storage+17
    jne failed
    movl $4, %eax
    movl $1, %ebx
    movl $message, %ecx
    movl $14, %edx
    int $0x80
    cmpl $14, %eax
    jne failed
    movl $4, %eax
    movl $99, %ebx
    int $0x80
    cmpl $-9, %eax
    jne failed
    movl $4, %eax
    movl $1, %ebx
    movl $1, %ecx
    int $0x80
    cmpl $-14, %eax
    jne failed
#ifdef NR_TLS_FAULT
    movl %gs:0, %eax
#endif
    movl $0x7fffffff, %eax
    int $0x80
    cmpl $-38, %eax
    jne failed
    movl $1, %eax
    movl $42, %ebx
    int $0x80
failed:
    movl $1, %eax
    movl $99, %ebx
    int $0x80
#endif
    .section .rodata
message:
    .ascii "native-elf-ok\n"
    .balign 16
simd_pattern:
    .quad 0x917203847566a7b8, 0xa1b2c3d4e5f60718
    .bss
    .balign 16
storage:
    .zero 8192

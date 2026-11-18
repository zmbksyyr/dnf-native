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

    .globl nr_thread_leave
nr_thread_leave:
    /* rcx=original Windows stack, rdx=run, r8=exit status. No guest
       stack/TLS access is permitted after finish publishes clear_tid. */
    movq %rcx, %rsp
    andq $-16, %rsp
    subq $32, %rsp
    movq %rdx, %rcx
    movq %r8, %rdx
    call nr_finish_thread
    ud2

    .globl nr_signal_enter
nr_signal_enter:
    /* VEH has returned before touching the guest's future signal frame.
       rax=handler, rdi=destination, rsi=staging, rcx=bytes,
       rdx=signal, r8=siginfo and r9=ucontext. */
    cld
    rep movsb
    movl %edx, %edi
    movq %r8, %rsi
    movq %r9, %rdx
    jmp *%rax
#else
    .text
    .globl _nr_sysinfo_gateway
_nr_sysinfo_gateway:
    /* The TLS CALL trampoline left [original target, guest return PC]. Only
       the verified loader gateway and these simple queries may run directly.
       Every other target/syscall retains its original INT 80 and context. */
    pushfl
    pushl %ecx
    movl _nr_sysinfo_pc, %ecx
    testl %ecx, %ecx
    jz 1f
    cmpl %ecx, 8(%esp)
    jne 1f
    cmpw $0x80cd, (%ecx)
    jne 1f
    cmpb $0xc3, 2(%ecx)
    jne 1f
    cmpl $20, %eax
    je 2f
    cmpl $224, %eax
    je 2f
    cmpl $13, %eax
    jne 1f
    testl %ebx, %ebx
    jne 1f
2:
    popl %ecx
    popfl
    pushfl
    pushal
    movl %esp, %ebp
    andl $-16, %esp
    subl $528, %esp
    fxsave 16(%esp)
    cld
    movl 28(%ebp), %eax
    movl %eax, (%esp)
    movl 16(%ebp), %eax
    movl %eax, 4(%esp)
    call _nr_fast_syscall
    testl %edx, %edx
    jz 3f
    movl %eax, 28(%ebp)
    fxrstor 16(%esp)
    movl %ebp, %esp
    popal
    popfl
    leal 4(%esp), %esp
    ret
3:
    fxrstor 16(%esp)
    movl %ebp, %esp
    popal
    popfl
    ret
1:
    popl %ecx
    popfl
    ret

    .globl _nr_enter
_nr_enter:
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    movl %edx, %esp
    xorl %ebp, %ebp
    xorl %edx, %edx
    cld
    jmp *%eax

    .globl _nr_thread_leave
_nr_thread_leave:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    movl 12(%esp), %edx
    movl %eax, %esp
    andl $-16, %esp
    subl $8, %esp
    pushl %edx
    pushl %ecx
    call _nr_finish_thread
    ud2

    .globl _nr_signal_enter
_nr_signal_enter:
    /* The cdecl arguments and restorer are part of the copied frame. */
    cld
    rep movsb
    jmp *%eax
#endif

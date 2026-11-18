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

/* TLS instructions execute on the CPU; check their return path and flags. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

#ifdef __x86_64__
#define TLS "%%fs:"
#define MOV "movq "
#define ADD "addq "
#define EXIT 60
#define WRITE 1
#else
#define TLS "%%gs:"
#define MOV "movl "
#define ADD "addl "
#define EXIT 1
#define WRITE 4
#endif

#ifndef NR_TLS_ROUNDS
#define NR_TLS_ROUNDS 10000
#endif

static result call(word n, word a, word b, word c) {
    result r;
#ifdef __x86_64__
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
#endif
    return r;
}

static void check(int condition, word line) {
    if (!condition) {
        call(EXIT, line, 0, 0);
        __builtin_unreachable();
    }
}
#define CHECK(c) check((c), __LINE__)

__attribute__((noinline)) static word target(void) { return 123; }
__attribute__((noinline)) static word target_other(void) { return 456; }

#ifndef __x86_64__
/* Observe the state on arrival, before a C prologue can change it. EDX holds
   the caller's ESP; a real indirect CALL contributes exactly one return word. */
__attribute__((naked, noinline)) static word target_state(void) {
    __asm__ volatile(
        "pushfl\n\tpopl %eax\n\tandl $0x41, %eax\n\tcmpl $0x41, %eax\n\tjne 1f\n\t"
        "cmpl $0x13579bdf, %ecx\n\tjne 1f\n\t"
        "leal 4(%esp), %eax\n\tcmpl %edx, %eax\n\tjne 1f\n\t"
        "movl $123, %eax\n\tret\n1:\txorl %eax, %eax\n\tret");
}
#endif

__attribute__((noinline)) static word tls_call(void) {
    word value;
#ifdef __x86_64__
    __asm__ volatile("call *" TLS "24" : "=a"(value) : : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
#else
    __asm__ volatile("call *" TLS "12" : "=a"(value) : : "ecx", "edx", "memory", "cc");
#endif
    return value;
}

/* A TLS jump must return to this function's caller, with no extra stack slot. */
__attribute__((naked, noinline)) static word tls_jump(void) {
#ifdef __x86_64__
    __asm__ volatile("jmp *%fs:24");
#else
    __asm__ volatile("jmp *%gs:12");
#endif
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    word tls[8] = {0};
    tls[3] = (word)target;
#ifdef __x86_64__
    CHECK(call(158, 0x1002, (word)tls, 0) == 0);
#else
    unsigned descriptor[4] = {~0u, (unsigned)(word)tls, 0xfffff, 0x51};
    CHECK(call(243, (word)descriptor, 0, 0) == 0);
    unsigned selector = descriptor[0] * 8 + 3;
    __asm__ volatile("movw %w0, %%gs" : : "r"(selector));
#endif
    word flags_ok;
    __asm__ volatile(
        "stc\n\t"
        MOV "$-1, " TLS "0\n\t"
        "jnc 1f\n\t"
        "lock " ADD "$1, " TLS "0\n\t"
        "jnc 1f\n\t"
        "jnz 1f\n\t"
        MOV "$1, %0\n\t"
        "jmp 2f\n1:\t"
        MOV "$0, %0\n2:"
        : "=r"(flags_ok) : : "memory", "cc");
    CHECK(flags_ok == 1 && tls[0] == 0);

#ifndef __x86_64__
    /* glibc's async-cancel state uses LOCK CMPXCHG with either ECX or EDX.
       Both success and failure must retain the accumulator, source, unrelated
       register and arithmetic flags. EAX as source is legal as well. */
    for (word i = 0; i < NR_TLS_ROUNDS; ++i) {
        word expected = i + 10, replacement = i + 20, preserved = i + 30, flags;
        tls[5] = expected;
        __asm__ volatile("lock cmpxchgl %%ecx, %%gs:20\n\tpushfl\n\tpopl %3"
                         : "+a"(expected), "+c"(replacement), "+d"(preserved), "=r"(flags) : : "memory", "cc");
        CHECK(expected == i + 10 && replacement == i + 20 && preserved == i + 30);
        CHECK(tls[5] == i + 20 && (flags & 0x8d5) == 0x44);
        expected = i + 10;
        __asm__ volatile("lock cmpxchgl %%edx, %%gs:20\n\tpushfl\n\tpopl %3"
                         : "+a"(expected), "+c"(preserved), "+d"(replacement), "=r"(flags) : : "memory", "cc");
        word native_flags, a = i + 10, b = i + 20;
        __asm__ volatile("cmpl %2, %1\n\tpushfl\n\tpopl %0"
                         : "=r"(native_flags) : "r"(a), "r"(b) : "cc");
        CHECK(expected == i + 20 && replacement == i + 20 && preserved == i + 30);
        CHECK(tls[5] == i + 20 && (flags & 0x8d5) == (native_flags & 0x8d5));
        __asm__ volatile("lock cmpxchgl %%eax, %%gs:20" : "+a"(expected) : : "memory", "cc");
        CHECK(expected == i + 20 && tls[5] == i + 20);
    }
    /* glibc errno ADD and cleanup-list MOV stores. Preserve unrelated EDX,
       MOV flags and carry/zero from real arithmetic, including TLS rebasing. */
    for (word i = 0; i < NR_TLS_ROUNDS; ++i) {
        word value=i+31, preserved=i+93, ok;
        __asm__ volatile(
            "xorl %%eax, %%eax\n\tstc\n\t"
            "movl %2, %%gs:4\n\tjnc 1f\n\tjnz 1f\n\t"
            "movl $-1, %%gs:0\n\tjnc 1f\n\tjnz 1f\n\t"
            "movl $1, %%ecx\n\taddl %%gs:0, %%ecx\n\tjnc 1f\n\tjnz 1f\n\t"
            "movl $1, %0\n\tjmp 2f\n1:\tmovl $0, %0\n2:"
            : "=&r"(ok), "+d"(preserved) : "r"(value) : "eax", "ecx", "memory", "cc");
        CHECK(ok==1 && tls[1]==value && tls[0]==(word)-1 && preserved==i+93);
    }
    /* Absolute glibc-style reads must preserve unrelated registers and MOV
       flags; CMP/XOR must publish the real CPU flags. ECX forces a different
       scratch register from the EAX/EDX cases. */
    tls[2] = 0x7fa53c19;
    for (word i = 0; i < 200000; ++i) {
        word loaded, preserved = i + 21, ok;
        __asm__ volatile(
            "xorl %%ecx, %%ecx\n\tstc\n\t"
            "movl %%gs:8, %%ecx\n\t"
            "jnc 1f\n\tjnz 1f\n\t"
            "xorl %%gs:8, %%ecx\n\t"
            "jnz 1f\n\tjc 1f\n\t"
            "cmpl $0x7fa53c19, %%gs:8\n\t"
            "jnz 1f\n\t"
            "movl $1, %1\n\tjmp 2f\n1:\tmovl $0, %1\n2:"
            : "=&c"(loaded), "=&r"(ok), "+d"(preserved) : : "memory", "cc");
        CHECK(loaded == 0 && ok == 1 && preserved == i + 21);
    }
    /* The direct path must read the current TLS base on every execution. */
    word other[8] = {0}; other[2] = 0x13579bdf; other[3] = (word)target_other;
    for (word i = 0; i < 8; ++i) {
        descriptor[1] = (unsigned)(word)((i & 1) ? tls : other);
        CHECK(call(243, (word)descriptor, 0, 0) == 0);
        word loaded;
        __asm__ volatile("movl %%gs:8, %0" : "=a"(loaded));
        CHECK(loaded == ((i & 1) ? tls[2] : other[2]));
        __asm__ volatile("movl %0, %%gs:4" : : "c"(i+71) : "memory");
        CHECK(((i & 1) ? tls[1] : other[1])==i+71);
        word expected=i, replacement=i+101;
        ((i & 1) ? tls : other)[5]=i;
        __asm__ volatile("lock cmpxchgl %%ecx, %%gs:20" : "+a"(expected) : "c"(replacement) : "memory", "cc");
        CHECK(expected==i && ((i & 1) ? tls : other)[5]==replacement);
        CHECK(tls_call() == ((i & 1) ? 123 : 456));
    }
    tls[3] = (word)target_state;
    word called, preserved = 0x13579bdf;
    __asm__ volatile(
        "movl %%esp, %%edx\n\txorl %%eax, %%eax\n\tstc\n\tcall *%%gs:12"
        : "=a"(called), "+c"(preserved) : : "edx", "memory", "cc");
    CHECK(called == 123 && preserved == 0x13579bdf);
    tls[3] = (word)target;
    for (word i = 0; i < NR_TLS_ROUNDS; ++i) {
        tls[3] = (i & 1) ? (word)target : (word)target_other;
        CHECK(tls_call() == ((i & 1) ? 123 : 456));
    }
    tls[3] = (word)target;
#else
    (void)target_other;
#endif

    /* Register-based addressing changes on each iteration; no stale address. */
    for (word i = 0; i < NR_TLS_ROUNDS; ++i) {
        word offset = (i & 1) * sizeof(word), value = i + 10, loaded;
        __asm__ volatile(
            MOV "%2, " TLS "(%1)\n\t"
            "lock " ADD "$1, " TLS "(%1)\n\t"
            MOV TLS "(%1), %0"
            : "=&r"(loaded) : "r"(offset), "r"(value) : "memory", "cc");
        CHECK(loaded == value + 1 && tls[i & 1] == value + 1);
    }
    CHECK(tls_call() == 123);
    CHECK(tls_jump() == 123);
    const char message[] = "native-tls-ok\n";
    CHECK(call(WRITE, 1, (word)message, sizeof(message) - 1) == sizeof(message) - 1);
    call(EXIT, 42, 0, 0);
    __builtin_unreachable();
}

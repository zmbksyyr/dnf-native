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
    word other[8] = {0}; other[2] = 0x13579bdf;
    for (word i = 0; i < 8; ++i) {
        descriptor[1] = (unsigned)(word)((i & 1) ? tls : other);
        CHECK(call(243, (word)descriptor, 0, 0) == 0);
        word loaded;
        __asm__ volatile("movl %%gs:8, %0" : "=a"(loaded));
        CHECK(loaded == ((i & 1) ? tls[2] : other[2]));
    }
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

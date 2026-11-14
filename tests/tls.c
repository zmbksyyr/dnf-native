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

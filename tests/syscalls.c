/* Freestanding Linux ABI checks, built for each architecture without libc. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

static result call(word n, word a, word b, word c) {
#ifdef __x86_64__
    result r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
#else
    result r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
#endif
}

static result call4(word n, word a, word b, word c, word d) {
#ifdef __x86_64__
    register word r10 __asm__("r10") = d;
    result r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
#else
    result r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
    return r;
}

static volatile word received;
static void handler(int signal) { received = (word)signal; }

#ifdef __x86_64__
enum {WRITE = 1, WRITEV = 20, BRK = 12, EXIT = 60, GETRLIMIT = 97, SETRLIMIT = 160};
#else
enum {WRITE = 4, WRITEV = 146, BRK = 45, EXIT = 1, GETRLIMIT = 191, SETRLIMIT = 75};
#endif

static void check(int condition, word line) {
    if (!condition) {
        call(EXIT, line, 0, 0);
        __builtin_unreachable();
    }
}
#define CHECK(c) check((c), __LINE__)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    const char left[] = "writev-", right[] = "ok\n";
    const word v[] = {1, 0, (word)left, 7, (word)right, 3};
    CHECK(call(WRITE, 0, 1, 0) == -9);
    CHECK(call(WRITE, 99, 1, 0) == -9);
    CHECK(call(WRITE, 1, 1, 0) == 0);
    CHECK(call(WRITEV, 0, 1, 0) == -9);
    CHECK(call(WRITEV, 1, 1, 0) == 0);
    CHECK(call(WRITEV, 1, (word)v, 1) == 0);
    CHECK(call(WRITEV, 1, 1, 1) == -14);
    CHECK(call(WRITEV, 1, (word)v, (word)-1) == -22);
    CHECK(call(WRITEV, 1, (word)v, 3) == 10);

    word limits[2] = {1, 1};
    CHECK(call(GETRLIMIT, 4, (word)limits, 0) == 0 && limits[0] == 0 && limits[1] == 0);
    CHECK(call(SETRLIMIT, 4, (word)limits, 0) == 0);
    limits[0] = 1;
    CHECK(call(SETRLIMIT, 4, (word)limits, 0) == -22);
    limits[1] = 1;
    CHECK(call(SETRLIMIT, 4, (word)limits, 0) == -38);

    word base = (word)call(BRK, 0, 0, 0);
    CHECK(base != 0 && (base & 4095) == 0);
    CHECK((word)call(BRK, base + 65537, 0, 0) == base + 65537);
    volatile unsigned char *heap = (volatile unsigned char *)base;
    for (word i = 0; i < 65537; ++i) CHECK(heap[i] == 0);
    heap[65536] = 55;
    CHECK((word)call(BRK, 0, 0, 0) == base + 65537);
    CHECK((word)call(BRK, base + 4097, 0, 0) == base + 4097);
    CHECK(call(WRITE, 1, base + 65536, 1) == -14);
    CHECK((word)call(BRK, base - 1, 0, 0) == base + 4097);
    CHECK((word)call(BRK, (word)-1, 0, 0) == base + 4097);
    CHECK((word)call(BRK, base + 65537, 0, 0) == base + 65537);
    CHECK(heap[65536] == 0);
    CHECK((word)call(BRK, base, 0, 0) == base);
    CHECK(call(WRITE, 1, base, 1) == -14);

    word tls[8] = {0};
    tls[2] = 123;
#ifdef __x86_64__
    CHECK(call(158, 0x1002, (word)tls, 0) == 0);
    word loaded;
    __asm__ volatile("movq %%fs:16, %0\n\tlock addq $7, %%fs:16" : "=r"(loaded) : : "memory", "cc");
#else
    unsigned descriptor[4] = {~0u, (unsigned)(word)tls, 0xfffff, 0x51};
    CHECK(call(243, (word)descriptor, 0, 0) == 0);
    unsigned selector = descriptor[0] * 8 + 3;
    __asm__ volatile("movw %w0, %%gs" : : "r"(selector));
    word loaded;
    __asm__ volatile("movl %%gs:8, %0\n\tlock addl $7, %%gs:8" : "=r"(loaded) : : "memory", "cc");
#endif
    CHECK(loaded == 123 && tls[2] == 130);
    word action[5] = {(word)handler, 0, 0, 0, 0};
#ifdef __x86_64__
    enum {ACTION = 13, MASK = 14, KILL = 62, GETPID = 39};
#else
    enum {ACTION = 174, MASK = 175, KILL = 37, GETPID = 20};
#endif
    CHECK(call4(ACTION, 15, (word)action, 0, 8) == 0);
    unsigned long long mask = 1ULL << 14;
    CHECK(call4(MASK, 0, (word)&mask, 0, 8) == 0);
    CHECK(call(KILL, (word)call(GETPID, 0, 0, 0), 15, 0) == 0);
    CHECK(received == 0);
    CHECK(call4(MASK, 1, (word)&mask, 0, 8) == 0);
    CHECK(received == 15);
    call(EXIT, 42, 0, 0);
    __builtin_unreachable();
}

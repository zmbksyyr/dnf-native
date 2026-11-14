/* Real kill() between independent guest processes, including a blocked signal. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word fourth __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(fourth) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
    return r;
}
#ifdef __x86_64__
enum {READ=0, WRITE=1, ACTION=13, MASK=14, KILL=62, EXIT=60};
#else
enum {READ=3, WRITE=4, ACTION=174, MASK=175, KILL=37, EXIT=1};
#endif
#define CHECK(value) do { if (!(value)) { call(EXIT, __LINE__, 0, 0, 0); __builtin_unreachable(); } } while (0)

#ifndef NR_TARGET
static volatile unsigned seen12, seen15;
static void handler(int signal) {
    if (signal == 12) ++seen12;
    if (signal == 15) ++seen15;
}
#endif

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
#ifdef NR_TARGET
    CHECK(call(KILL, NR_TARGET, 0, 0, 0) == 0);
    CHECK(call(KILL, NR_TARGET, 12, 0, 0) == 0);
    CHECK(call(KILL, NR_TARGET, 15, 0, 0) == 0);
#else
    struct { word handler, flags, restorer; unsigned long long mask; } action = {(word)handler, 0, 0, 0};
    unsigned long long mask = 1ULL << 11;
    CHECK(call(ACTION, 12, (word)&action, 0, 8) == 0);
    CHECK(call(ACTION, 15, (word)&action, 0, 8) == 0);
    CHECK(call(MASK, 0, (word)&mask, 0, 8) == 0);
    CHECK(call(WRITE, 1, (word)"signal-ready\n", 13, 0) == 13);
    char release;
    CHECK(call(READ, 0, (word)&release, 1, 0) == 1 && release == 'u');
    CHECK(seen12 == 0 && seen15 == 1);
    CHECK(call(MASK, 1, (word)&mask, 0, 8) == 0);
    CHECK(seen12 == 1 && seen15 == 1);
#endif
    call(EXIT, 42, 0, 0, 0);
    __builtin_unreachable();
}

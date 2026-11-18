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
#define CHECK(value) do { if (!(value)) { call(EXIT, 100 + __LINE__ % 100, 0, 0, 0); __builtin_unreachable(); } } while (0)

#ifndef NR_TARGET
static volatile unsigned seen12, seen15;
__attribute__((naked)) static void restore(void) {
#ifdef __x86_64__
    __asm__ volatile("mov $15, %rax; syscall");
#else
    __asm__ volatile("mov $173, %eax; int $0x80");
#endif
}
static void handler(int signal, void *info, void *context) {
    (void)info; (void)context;
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
    struct { word handler, flags, restorer; unsigned long long mask; } action = {(word)handler, 0x04000004, (word)restore, 0};
    unsigned long long mask = 1ULL << 11;
    CHECK(call(ACTION, 12, (word)&action, 0, 8) == 0);
    CHECK(call(ACTION, 15, (word)&action, 0, 8) == 0);
    /* Repeat the block/send/unblock handshake to catch lost notifications
       across successive external events, rather than only initial delivery. */
    for (unsigned round = 1; round <= 16; ++round) {
        CHECK(call(MASK, 0, (word)&mask, 0, 8) == 0);
        CHECK(call(WRITE, 1, (word)"signal-ready\n", 13, 0) == 13);
        char release;
        result read;
        do { read = call(READ, 0, (word)&release, 1, 0); } while (read == -4);
        CHECK(read == 1 && release == 'u');
        CHECK(seen12 == round - 1 && seen15 == round);
        CHECK(call(MASK, 1, (word)&mask, 0, 8) == 0);
        CHECK(seen12 == round && seen15 == round);
    }
#endif
    call(EXIT, 42, 0, 0, 0);
    __builtin_unreachable();
}

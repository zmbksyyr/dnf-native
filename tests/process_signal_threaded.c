/* Process signals prefer the group leader; a masked leader permits a worker. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
extern int pthread_create(word *, const void *, void *(*)(void *), void *);
extern int pthread_join(word, void **);
extern int __libc_start_main(int (*)(void), int, char **, void *, void *, void *, void *);
extern void _exit(int) __attribute__((noreturn));
#define CHECK(c) do { if (!(c)) _exit(__LINE__); } while (0)
static volatile unsigned stop, seen, handler_tid;

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
enum {READ=0, WRITE=1, ACTION=13, MASK=14, GETPID=39, GETTID=186};
#else
enum {READ=3, WRITE=4, ACTION=174, MASK=175, GETPID=20, GETTID=224};
#endif
static void handler(int signal) {
    CHECK(signal == 12);
    handler_tid = (unsigned)call(GETTID, 0, 0, 0, 0);
    ++seen;
}
static void *worker(void *unused) {
    (void)unused;
    unsigned long long mask = 1ULL << 11;
    CHECK(call(MASK, 1, (word)&mask, 0, 8) == 0);
    while (!stop) call(GETPID, 0, 0, 0, 0);
    return 0;
}
static int main_test(void) {
    struct { word handler, flags, restorer; unsigned long long mask; } action = {(word)handler, 0, 0, 0};
    CHECK(call(ACTION, 12, (word)&action, 0, 8) == 0);
#ifdef NR_BLOCK_LEADER
    unsigned long long mask = 1ULL << 11;
    CHECK(call(MASK, 0, (word)&mask, 0, 8) == 0);
#endif
    word thread;
    unsigned leader = (unsigned)call(GETTID, 0, 0, 0, 0);
    CHECK(pthread_create(&thread, 0, worker, 0) == 0);
    CHECK(call(WRITE, 1, (word)"thread-signal-ready\n", 20, 0) == 20);
    char release;
    CHECK(call(READ, 0, (word)&release, 1, 0) == 1);
    CHECK(seen == 1);
#ifdef NR_BLOCK_LEADER
    CHECK(handler_tid != leader);
#else
    CHECK(handler_tid == leader);
#endif
    stop = 1;
    CHECK(pthread_join(thread, 0) == 0);
    return 42;
}
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    __libc_start_main(main_test, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* Exercise queue movement and conditional atomic wakes with real guest threads. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

#ifdef __x86_64__
enum {FUTEX=202, EXIT=60, GROUP_EXIT=231, WRITE=1, SLEEP=35, ROBUST=273};
#else
enum {FUTEX=240, EXIT=1, GROUP_EXIT=252, WRITE=4, SLEEP=162, ROBUST=311};
#endif

static result call(word n, word a, word b, word c, word d, word e, word f) {
    result value;
#ifdef __x86_64__
    register word r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(value) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
#else
    register word bp __asm__("ebp") = f;
    __asm__ volatile("int $0x80" : "=a"(value) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "r"(bp) : "memory");
#endif
    return value;
}

#define CHECK(c) do { if (!(c)) { call(GROUP_EXIT, __LINE__, 0, 0, 0, 0, 0); __builtin_unreachable(); } } while (0)

static result futex(unsigned *address, word operation, word count, word second_count, unsigned *second, word value) {
    return call(FUTEX, (word)address, operation, count, second_count, (word)second, value);
}

static void delay(void) {
    result duration[2] = {0, 1000000};
    CHECK(call(SLEEP, (word)duration, 0, 0, 0, 0, 0) == 0);
}

#ifndef ROBUST_FIXTURE
static unsigned first, second;
#endif
struct Worker {
    unsigned char stack[1024 * 1024] __attribute__((aligned(16)));
    word tls[8];
    unsigned descriptor[4], tid, done;
#ifdef ROBUST_FIXTURE
    word head[3];
    struct { unsigned owner; word next; } locks[3];
    unsigned ready, go;
#endif
};
static struct Worker workers[9];
extern result spawn(word stack, word parent_tid, word tls, word child_tid);

__attribute__((noreturn)) void child_main(struct Worker *worker) {
#ifdef ROBUST_FIXTURE
    unsigned index = (unsigned)(worker - workers);
    word head = (word)worker->head;
    word node = (word)&worker->locks[0].next;
    worker->head[0] = node;
    worker->head[1] = (word)&worker->locks[0].owner - node; /* glibc uses a negative futex_offset. */
    worker->locks[0].next = head;
    worker->locks[0].owner = worker->tid | 0x80000000u;
    if (index == 0) {
        worker->locks[0].next = (word)&worker->locks[1].next;
        worker->locks[1].next = (word)&worker->locks[2].next;
        worker->locks[2].next = head;
        worker->locks[2].owner = worker->tid ^ 0x100000;
    }
    if (index == 1 || index == 8) { worker->head[0] = head; worker->head[2] = node; }
    if (index == 2) worker->head[2] = node; /* Pending item already linked: process once. */
    if (index == 3) head = 1;
    if (index == 4) worker->head[0] = 2;
    if (index == 5) worker->locks[0].next = node; /* Malformed cycle is bounded. */
    if (index == 6) worker->head[0] |= 1; /* PI cannot pretend to be a normal mutex. */
    if (index == 7) worker->locks[0].owner ^= 0x100000;
    if (index == 8) worker->locks[0].owner = 0x80000000u;
    CHECK(call(ROBUST, head, 3 * sizeof(word) - 1, 0, 0, 0, 0) == -22);
    CHECK(call(ROBUST, head, 3 * sizeof(word), 0, 0, 0, 0) == 0);
    __atomic_store_n(&worker->ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&worker->go, __ATOMIC_SEQ_CST)) delay();
    delay();
#else
    for (unsigned i = 0; i < 2000; ++i) {
        word self;
#ifdef __x86_64__
        __asm__ volatile("movq %%fs:0, %0" : "=r"(self));
#else
        __asm__ volatile("movl %%gs:0, %0" : "=r"(self));
#endif
        CHECK(self == (word)worker->tls);
    }
    CHECK(futex(&first, 128, 0, 0, 0, 0) == 0);
#endif
    __atomic_store_n(&worker->done, 1, __ATOMIC_SEQ_CST);
    call(EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static void start(unsigned index) {
    struct Worker *w = &workers[index];
    word top = (word)(w->stack + sizeof(w->stack)) - 16;
    *(word *)top = (word)w;
    w->tls[0] = (word)w->tls;
    w->descriptor[0] = 12; w->descriptor[1] = (unsigned)(word)w->tls;
    w->descriptor[2] = 0xfffff; w->descriptor[3] = 0x51;
#ifdef __x86_64__
    word tls = (word)w->tls;
#else
    word tls = (word)w->descriptor;
#endif
    CHECK(spawn(top, (word)&w->tid, tls, (word)&w->tid) > 0);
}

#ifndef ROBUST_FIXTURE
static void move_all(unsigned count) {
    unsigned moved = 0;
    for (unsigned tries = 0; moved < count && tries < 3000; ++tries) {
        result n = futex(&first, 132, 0, count - moved, &second, 0);
        CHECK(n >= 0);
        moved += (unsigned)n;
        if (moved < count) delay();
    }
    CHECK(moved == count);
}
#endif

static void join(unsigned begin, unsigned count) {
    for (unsigned i = begin; i < begin + count; ++i) {
        for (unsigned tries = 0; __atomic_load_n(&workers[i].tid, __ATOMIC_SEQ_CST) && tries < 3000; ++tries) delay();
        CHECK(__atomic_load_n(&workers[i].tid, __ATOMIC_SEQ_CST) == 0);
        CHECK(__atomic_load_n(&workers[i].done, __ATOMIC_SEQ_CST) == 1);
    }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
#ifdef ROBUST_FIXTURE
    CHECK(call(ROBUST, 0, 3 * sizeof(word), 0, 0, 0, 0) == 0);
    unsigned real_wakes = 0;
    for (unsigned index = 0; index < 9; ++index) {
        start(index);
        struct Worker *w = &workers[index];
        while (!__atomic_load_n(&w->ready, __ATOMIC_SEQ_CST)) delay();
        unsigned old = w->locks[0].owner;
        __atomic_store_n(&w->go, 1, __ATOMIC_SEQ_CST);
        if (index < 3 || index == 8) {
            result timeout[2] = {2, 0};
            result waited = futex(&w->locks[0].owner, 128, old, (word)timeout, 0, 0);
            CHECK(waited == 0 || waited == -11);
            if (!waited) ++real_wakes;
        }
        join(index, 1);
        unsigned expected = index < 3 || index == 5 ? 0xc0000000u : old;
        CHECK(w->locks[0].owner == expected);
        if (!index) CHECK(w->locks[1].owner == 0 && w->locks[2].owner != 0xc0000000u);
    }
    CHECK(real_wakes > 0);
    const char message[] = "native-robust-owner-death-ok\n";
#else
    for (unsigned i = 0; i < 3; ++i) start(i);
    move_all(3);
    CHECK(futex(&second, 132, 1, 2, &first, 1) == -11); /* Compare failure leaves every waiter asleep. */
    CHECK(futex(&second, 132, 1, 2, &first, 0) == 3);
    CHECK(futex(&second, 129, 9, 0, 0, 0) == 0);
    CHECK(futex(&first, 129, 9, 0, 0, 0) == 2);
    join(0, 3);
    for (unsigned round = 0; round < 2; ++round) {
        unsigned begin = 3 + round * 2;
        start(begin); start(begin + 1);
        move_all(2);
        CHECK(futex(&second, 132, 0, 1, &first, 0) == 1);
        __atomic_store_n(&second, round ? 1 : 2, __ATOMIC_SEQ_CST);
        /* SET second=0, compare its old value >1, wake first and conditionally second. */
        CHECK(futex(&first, 133, 1, 1, &second, 0x04000001) == (round ? 1 : 2));
        CHECK(__atomic_load_n(&second, __ATOMIC_SEQ_CST) == 0);
        CHECK(futex(&second, 129, 9, 0, 0, 0) == (round ? 1 : 0));
        join(begin, 2);
    }
    second = 7;
    CHECK(futex(&first, 133, 0, 0, &second, 0x10002000) == 0 && second == 9); /* ADD 2. */
    CHECK(futex(&first, 133, 0, 0, &second, 0x20004000) == 0 && second == 13); /* OR 4. */
    CHECK(futex(&first, 133, 0, 0, &second, 0x30008000) == 0 && second == 5); /* AND NOT 8. */
    CHECK(futex(&first, 133, 0, 0, &second, 0x40003000) == 0 && second == 6); /* XOR 3. */
    CHECK(futex(&first, 133, 0, 0, &second, 0x80004000) == 0 && second == 16); /* SET 1<<4. */
    CHECK(futex(&first, 133, 0, 0, &second, 0x000ff000) == 0 && second == 255);
    CHECK(futex(&first, 133, 0, 0, &second, 0x00fff000) == 0 && second == (unsigned)-1);
    const char message[] = "native-futex-requeue-wake-op-ok\n";
#endif
    CHECK(call(WRITE, 1, (word)message, sizeof(message)-1, 0, 0, 0) == sizeof(message)-1);
    call(GROUP_EXIT, 42, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

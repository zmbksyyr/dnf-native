/* Shared Linux descriptor allocation and independent epoll sets across threads. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

#ifdef __x86_64__
enum {READ=0, WRITE=1, OPEN=2, CLOSE=3, CREATE=291, CTL=233, WAIT=232,
      EVENT=290, PIPE=293, SELECT=23, POLL=7, GETLIMIT=97, EXIT=60, GROUP_EXIT=231};
#else
enum {READ=3, WRITE=4, OPEN=5, CLOSE=6, CREATE=329, CTL=255, WAIT=256,
      EVENT=328, PIPE=331, SELECT=142, POLL=168, GETLIMIT=191, EXIT=1, GROUP_EXIT=252};
#endif

static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
    return r;
}

#define CHECK(c) do { if (!(c)) { call(GROUP_EXIT, __LINE__, 0, 0, 0); __builtin_unreachable(); } } while (0)
enum {THREADS=8, ROUNDS=200};
struct Event { unsigned events; unsigned long long data; } __attribute__((packed));
struct Worker {
    unsigned char stack[1024 * 1024] __attribute__((aligned(16)));
    word tls[8];
    unsigned descriptor[4], tid;
    result fds[4];
    unsigned count;
};
static struct Worker workers[THREADS];
static unsigned arrived, phase;
static unsigned capacity;
extern result spawn(word stack, word parent_tid, word tls, word child_tid);

void *memset(void *destination, int value, __SIZE_TYPE__ count) {
    volatile unsigned char *out = destination;
    while (count--) *out++ = (unsigned char)value;
    return destination;
}

static void barrier(unsigned next) {
    __atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&phase, __ATOMIC_SEQ_CST) < next) __asm__ volatile("pause");
}

__attribute__((noreturn)) void child_main(struct Worker *w) {
    unsigned index = (unsigned)(w - workers);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        barrier(3 * round + 1);
        w->fds[0] = call(CREATE, 0, 0, 0, 0);
        w->fds[1] = call(EVENT, index + 1, 0x800, 0, 0);
        unsigned kind = (round + index) % 6;
        w->count = 3;
        if (kind == 0) {
            int pipe[2];
            CHECK(call(PIPE, (word)pipe, 0x800, 0, 0) == 0);
            w->fds[2] = pipe[0]; w->fds[3] = pipe[1]; w->count = 4;
        } else if (kind == 1) {
#ifdef __x86_64__
            w->fds[2] = call(41, 2, 2, 0, 0);
#else
            word args[] = {2, 2, 0};
            w->fds[2] = call(102, 1, (word)args, 0, 0);
#endif
        } else {
            const char *path = kind == 2 ? "/regular" : kind == 3 ? "/proc/self/maps" :
                               kind == 4 ? "/dev/urandom" : "/proc/self/auxv";
            w->fds[2] = call(OPEN, (word)path, 0, 0, 0);
        }
        for (unsigned i = 0; i < w->count; ++i) CHECK(w->fds[i] >= 515 && w->fds[i] < (result)capacity);
        struct Event input = {1, ((unsigned long long)round << 32) | (index + 1)}, output[2];
        CHECK(call(CTL, w->fds[0], 1, w->fds[1], (word)&input) == 0);
        barrier(3 * round + 2); /* Keep every descriptor open for the uniqueness check. */
        CHECK(call(WAIT, w->fds[0], (word)output, 2, 0) == 1);
        CHECK(output[0].events == 1 && output[0].data == input.data);
        unsigned long long value = 0;
        CHECK(call(READ, w->fds[1], (word)&value, 8, 0) == 8 && value == index + 1);
        for (unsigned i = 0; i < w->count; ++i) CHECK(call(CLOSE, w->fds[i], 0, 0, 0) == 0);
        barrier(3 * round + 3);
    }
    call(EXIT, 0, 0, 0, 0);
    __builtin_unreachable();
}

static result select_ready(word count, word *reading, word *writing) {
    word timeout[2] = {0, 0};
    result value;
#ifdef __x86_64__
    register word fourth __asm__("r10") = 0;
    register word fifth __asm__("r8") = (word)timeout;
    __asm__ volatile("syscall" : "=a"(value) : "a"((word)SELECT), "D"(count), "S"(reading),
                     "d"(writing), "r"(fourth), "r"(fifth) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(value) : "a"((word)SELECT), "b"(count), "c"(reading),
                     "d"(writing), "S"((word)0), "D"(timeout) : "memory");
#endif
    return value;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    word limits[2];
    CHECK(call(GETLIMIT, 7, (word)limits, 0, 0) == 0);
    CHECK(limits[0] >= 1007 && limits[0] <= 4096 && limits[0] == limits[1]);
    capacity = (unsigned)limits[0];
    /* Keep the concurrent mixed allocations above the old 256-slot boundary. */
    for (unsigned fd = 3; fd < 515; ++fd) CHECK(call(EVENT, 0, 0, 0, 0) == (result)fd);
    for (unsigned i = 0; i < THREADS; ++i) {
        struct Worker *w = &workers[i];
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
    for (unsigned step = 1; step <= 3 * ROUNDS; ++step) {
        while (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < THREADS * step) __asm__ volatile("pause");
        if (step % 3 == 2) {
            unsigned char used[4096] = {0};
            for (unsigned i = 0; i < THREADS; ++i) for (unsigned j = 0; j < workers[i].count; ++j) {
                unsigned fd = (unsigned)workers[i].fds[j];
                CHECK(!used[fd]); used[fd] = 1;
            }
        }
        __atomic_store_n(&phase, step, __ATOMIC_SEQ_CST);
    }
    for (unsigned i = 0; i < THREADS; ++i)
        while (__atomic_load_n(&workers[i].tid, __ATOMIC_SEQ_CST)) __asm__ volatile("pause");
    result ep = call(CREATE, 0, 0, 0, 0), sock;
#ifdef __x86_64__
    sock = call(41, 2, 2, 0, 0);
#else
    word socket_args[] = {2, 2, 0};
    sock = call(102, 1, (word)socket_args, 0, 0);
#endif
    CHECK(ep >= 515 && sock >= 515 && ep != sock);
    unsigned long long value = 7;
    CHECK(call(WRITE, 514, (word)&value, 8, 0) == 8);
    struct { int fd; short events, revents; } polls[4096];
    for (unsigned i = 0; i < capacity; ++i) polls[i].fd = -1;
    polls[0].fd = 514; polls[0].events = 1;
    polls[capacity - 1].fd = (int)sock; polls[capacity - 1].events = 4;
    CHECK(call(POLL, (word)polls, capacity, 0, 0) == 2);
    CHECK(polls[0].revents == 1 && polls[capacity - 1].revents == 4);
    word read_bits[4096 / (8 * sizeof(word))] = {0}, write_bits[4096 / (8 * sizeof(word))] = {0};
    unsigned bits = 8 * sizeof(word);
    write_bits[sock / bits] = (word)1 << (sock % bits);
    CHECK(select_ready((word)sock + 1, 0, write_bits) == 1); /* Winsock-only select. */
    CHECK(write_bits[sock / bits] == (word)1 << (sock % bits));
    read_bits[514 / bits] = (word)1 << (514 % bits);
    CHECK(select_ready((word)sock + 1, read_bits, write_bits) == 2); /* Mixed eventfd select. */
    CHECK(read_bits[514 / bits] == (word)1 << (514 % bits));
    struct Event input = {1, 0x514}, output[2];
    CHECK(call(CTL, ep, 1, 514, (word)&input) == 0);
    input.events = 4; input.data = 0x515;
    CHECK(call(CTL, ep, 1, sock, (word)&input) == 0);
    CHECK(call(WAIT, ep, (word)output, 2, 0) == 2);
    CHECK(output[0].events == 1 && output[0].data == 0x514);
    CHECK(output[1].events == 4 && output[1].data == 0x515);
    CHECK(call(CLOSE, ep, 0, 0, 0) == 0 && call(CLOSE, sock, 0, 0, 0) == 0);
    for (unsigned fd = 3; fd < 515; ++fd) CHECK(call(CLOSE, fd, 0, 0, 0) == 0);
    CHECK(call(OPEN, (word)"/missing", 0, 0, 0) == -2);
    for (unsigned fd = 3; fd < capacity - 1; ++fd) CHECK(call(EVENT, 0, 0, 0, 0) == (result)fd);
    int pipe[2];
    CHECK(call(PIPE, (word)pipe, 0, 0, 0) == -24); /* One slot left: roll back both reservations. */
    CHECK(call(CREATE, 0, 0, 0, 0) == (result)capacity - 1);
    CHECK(call(CREATE, 0, 0, 0, 0) == -24);
    for (unsigned fd = 3; fd < capacity; ++fd) CHECK(call(CLOSE, fd, 0, 0, 0) == 0);
    const char message[] = "native-descriptor-allocation-isolation-ok\n";
    CHECK(call(WRITE, 1, (word)message, sizeof(message) - 1, 0) == sizeof(message) - 1);
    call(GROUP_EXIT, 42, 0, 0, 0);
    __builtin_unreachable();
}

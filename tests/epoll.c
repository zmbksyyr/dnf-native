/* Pipe/socket epoll readiness and Linux event data; no libc. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

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

#ifdef __x86_64__
enum {READ=0, WRITE=1, CLOSE=3, PIPE2=293, CLOCK=228, CREATE=291, CTL=233, WAIT=232, EXIT=60};
#else
enum {READ=3, WRITE=4, CLOSE=6, PIPE2=331, CLOCK=265, CREATE=329, CTL=255, WAIT=256, EXIT=1};
#endif

#define CHECK(c) do { if (!(c)) { call(EXIT, __LINE__, 0, 0, 0); __builtin_unreachable(); } } while (0)
struct Event { unsigned events; unsigned long long data; } __attribute__((packed));

static result net(unsigned operation, word a, word b, word c) {
#ifdef __x86_64__
    return call(operation == 1 ? 41 : 42, a, b, c, 0);
#else
    word args[] = {a,b,c};
    return call(102, operation, (word)args, 0, 0);
#endif
}

static void elapsed(result before[2]) {
    result after[2];
    CHECK(call(CLOCK, 1, (word)after, 0, 0) == 0);
    result ms = (after[0] - before[0]) * 1000 + (after[1] - before[1]) / 1000000;
    CHECK(ms >= 70 && ms < 3000);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int pipe[2]; char byte;
    result before[2];
    CHECK(call(PIPE2, (word)pipe, 0x80800, 0, 0) == 0);
    result epoll = call(CREATE, 0x80000, 0, 0, 0);
    CHECK(epoll >= 0);
    struct Event input = {1, 0x12345678fedcba98ULL}, output[2];
    CHECK(call(CTL, epoll, 1, pipe[0], (word)&input) == 0);
    CHECK(call(CLOCK, 1, (word)before, 0, 0) == 0);
    CHECK(call(WAIT, epoll, (word)output, 2, 100) == 0);
    elapsed(before);
    CHECK(call(WRITE, pipe[1], (word)"p", 1, 0) == 1);
    CHECK(call(WAIT, epoll, (word)output, 2, 0) == 1);
    CHECK(output[0].events == 1 && output[0].data == input.data);
    input.events = 0;
    CHECK(call(CTL, epoll, 3, pipe[0], (word)&input) == 0);
    CHECK(call(WAIT, epoll, (word)output, 2, 0) == 0);
    CHECK(call(READ, pipe[0], (word)&byte, 1, 0) == 1 && byte == 'p');
    input.events = 1;
    CHECK(call(CTL, epoll, 3, pipe[0], (word)&input) == 0);

    struct { unsigned short family, port; unsigned address; unsigned char zero[8]; } address =
        {2, (NR_PORT >> 8) | ((NR_PORT & 255) << 8), 0x0100007f, {0}};
    result socket = net(1, 2, 2, 0);
    CHECK(socket >= 0);
    CHECK(net(3, socket, (word)&address, sizeof(address)) == 0);
    input.data = 0xfedcba9876543210ULL;
    CHECK(call(CTL, epoll, 1, socket, (word)&input) == 0);
    CHECK(call(WRITE, socket, (word)"u", 1, 0) == 1);
    CHECK(call(WAIT, epoll, (word)output, 2, 2000) == 1);
    CHECK(output[0].events == 1 && output[0].data == input.data);
    CHECK(call(READ, socket, (word)&byte, 1, 0) == 1 && byte == 'v');
    CHECK(call(CLOSE, socket, 0, 0, 0) == 0);

    CHECK(call(CLOSE, pipe[1], 0, 0, 0) == 0);
    CHECK(call(WAIT, epoll, (word)output, 2, 0) == 1);
    CHECK(output[0].events == 16 && output[0].data == 0x12345678fedcba98ULL);
    CHECK(call(CTL, epoll, 2, pipe[0], 0) == 0);
    CHECK(call(WAIT, epoll, (word)output, 2, 0) == 0);
    input.data = 9;
    CHECK(call(CTL, epoll, 1, pipe[0], (word)&input) == 0);
    CHECK(call(CLOSE, pipe[0], 0, 0, 0) == 0);
    CHECK(call(PIPE2, (word)pipe, 0x80800, 0, 0) == 0);
    CHECK(call(WAIT, epoll, (word)output, 2, 0) == 0); /* Closed fd's watch is gone. */
    CHECK(call(CLOSE, pipe[0], 0, 0, 0) == 0);
    CHECK(call(CLOSE, pipe[1], 0, 0, 0) == 0);

    /* A permanently readable low fd must not hide later ready descriptors
       when maxevents is smaller than the ready set (epoll(7) round robin). */
    int fair[6][2];
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(call(PIPE2, (word)fair[i], 0x80800, 0, 0) == 0);
        input.events = 1; input.data = i;
        CHECK(call(CTL, epoll, 1, fair[i][0], (word)&input) == 0);
        CHECK(call(WRITE, fair[i][1], (word)"f", 1, 0) == 1);
    }
    unsigned seen = 0;
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(call(WAIT, epoll, (word)output, 2, 0) == 2);
        for (unsigned j = 0; j < 2; ++j) {
            CHECK(output[j].data < 6);
            seen |= 1u << output[j].data;
        }
    }
    CHECK(seen == 63);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(call(CLOSE, fair[i][0], 0, 0, 0) == 0);
        CHECK(call(CLOSE, fair[i][1], 0, 0, 0) == 0);
    }

    input.data = 0xabcdef0123456789ULL;
    CHECK(call(CTL, epoll, 1, 0, (word)&input) == 0);
    CHECK(call(WRITE, 1, (word)"epoll-wait\n", 11, 0) == 11);
    CHECK(call(CLOCK, 1, (word)before, 0, 0) == 0);
    CHECK(call(WAIT, epoll, (word)output, 2, (word)-1) == 1);
    elapsed(before);
    CHECK(output[0].events == 1 && output[0].data == input.data);
    CHECK(call(READ, 0, (word)&byte, 1, 0) == 1 && byte == 'g');
    CHECK(call(CLOSE, epoll, 0, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)"pipe-epoll-ok\n", 14, 0) == 14);
    call(EXIT, 42, 0, 0, 0);
    __builtin_unreachable();
}

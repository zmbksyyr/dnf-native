/* Real TCP byte exchange and Linux poll/timeout ABI checks; no libc. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

static result call(word n, word a, word b, word c) {
    result r;
#ifdef __x86_64__
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
#endif
    return r;
}

static result net(unsigned operation, word a, word b, word c, word d, word e) {
#ifdef __x86_64__
    word number = operation == 1 ? 41 : operation == 3 ? 42 : 54;
    register word r10 __asm__("r10") = d;
    register word r8 __asm__("r8") = e;
    result r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
#else
    word args[] = {a, b, c, d, e};
    return call(102, operation, (word)args, 0);
#endif
}

#ifdef __x86_64__
enum {READ=0, WRITE=1, CLOSE=3, IOCTL=16, POLL=7, CLOCK=228, PIPE2=293, EXIT=60};
#else
enum {READ=3, WRITE=4, CLOSE=6, IOCTL=54, POLL=168, CLOCK=265, PIPE2=331, EXIT=1};
#endif

static void check(int condition, word line) {
    if (!condition) { call(EXIT, line, 0, 0); __builtin_unreachable(); }
}
#define CHECK(c) check((c), __LINE__)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    const char request[] = "native-tcp\0guest", expected[] = "native-tcp\0reply";
    char reply[sizeof(expected) - 1];
    struct { unsigned short family, port; unsigned address; unsigned char zero[8]; } address =
        {2, (NR_PORT >> 8) | ((NR_PORT & 255) << 8), 0x0100007f, {0}};
    struct { int fd; short events, revents; } poll, mixed[2];
    int report[14] = {0}, one = 1;
    result fd = net(1, 2, 1, 0, 0, 0);
    CHECK(fd >= 0);
    struct { char name[16]; unsigned short family; unsigned char address[14]; } ifr = {"eth0", 0, {0}};
    CHECK(call(IOCTL, fd, 0x8927, (word)&ifr) == 0);
    CHECK(ifr.family == 1 && (ifr.address[0] | ifr.address[1] | ifr.address[2] |
                             ifr.address[3] | ifr.address[4] | ifr.address[5]) != 0);
    report[11] = (int)net(14, fd, 1, 9, (word)&one, sizeof(one));
    report[12] = (int)net(14, fd, 6, 1, (word)&one, sizeof(one));
    CHECK(report[11] == 0 && report[12] == 0);
    CHECK(net(3, fd, (word)&address, sizeof(address), 0, 0) == 0);

    poll.fd = (int)fd; poll.events = 1; poll.revents = 99;
    report[0] = (int)call(POLL, (word)&poll, 1, 0);
    CHECK(report[0] == 0 && poll.revents == 0);
    poll.events = 4;
    report[1] = (int)call(POLL, (word)&poll, 1, 0); report[2] = poll.revents;
    CHECK(report[1] == 1 && report[2] == 4);
    poll.fd = -1; poll.events = 5; poll.revents = 99;
    report[3] = (int)call(POLL, (word)&poll, 1, 0); report[4] = poll.revents;
    CHECK(report[3] == 0 && report[4] == 0);
    poll.fd = 255; poll.events = 1; poll.revents = 99;
    report[5] = (int)call(POLL, (word)&poll, 1, 1000); report[6] = poll.revents;
    CHECK(report[5] == 1 && report[6] == 32);

    word sent = 0;
    while (sent < sizeof(request) - 1) {
        result count = call(WRITE, fd, (word)(request + sent), sizeof(request) - 1 - sent);
        CHECK(count > 0); sent += (word)count;
    }
    int pipe[2];
    CHECK(call(PIPE2, (word)pipe, 0x80800, 0) == 0);
    mixed[0].fd = (int)fd; mixed[0].events = 1;
    mixed[1].fd = pipe[0]; mixed[1].events = 1;
    report[7] = (int)call(POLL, (word)mixed, 2, 5000); report[8] = mixed[0].revents;
    CHECK(report[7] == 1 && report[8] == 1 && mixed[1].revents == 0);
    CHECK(call(CLOSE, pipe[0], 0, 0) == 0 && call(CLOSE, pipe[1], 0, 0) == 0);
    word received = 0;
    while (received < sizeof(reply)) {
        result count = call(READ, fd, (word)(reply + received), sizeof(reply) - received);
        CHECK(count > 0); received += (word)count;
    }
    for (word i = 0; i < sizeof(reply); ++i) CHECK(reply[i] == expected[i]);

    result timeout[] = {31536000, 0}, short_timeout[] = {0, 100000}, before[2], after[2];
    CHECK(net(14, fd, 1, 20, (word)timeout, sizeof(timeout)) == 0);
    report[13] = (int)net(14, fd, 1, 20, (word)short_timeout, sizeof(short_timeout));
    CHECK(report[13] == 0);
    CHECK(call(CLOCK, 1, (word)before, 0) == 0);
    report[9] = (int)call(READ, fd, (word)reply, 1);
    CHECK(call(CLOCK, 1, (word)after, 0) == 0);
    report[10] = (int)((after[0] - before[0]) * 1000 + (after[1] - before[1]) / 1000000);
    CHECK(report[9] == -11 && report[10] >= 50 && report[10] < 3000);
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)report, sizeof(report)) == sizeof(report));
    CHECK(call(WRITE, 1, (word)reply, sizeof(reply)) == sizeof(reply));
    call(EXIT, 42, 0, 0);
    __builtin_unreachable();
}

/* Pipe state and timed wakeups through Linux poll, using real kernel pipes. */
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

#ifdef __x86_64__
enum {READ=0, WRITE=1, CLOSE=3, PIPE2=293, POLL=7, CLOCK=228, EXIT=60};
#else
enum {READ=3, WRITE=4, CLOSE=6, PIPE2=331, POLL=168, CLOCK=265, EXIT=1};
#endif

static void check(int condition, word line) {
    if (!condition) { call(EXIT, line, 0, 0); __builtin_unreachable(); }
}
#define CHECK(c) check((c), __LINE__)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int fds[2];
    struct { int fd; short events, revents; } poll;
    static char bytes[4096];
    result before[2], after[2];
    CHECK(call(PIPE2, (word)fds, 0x80800, 0) == 0);
    poll.fd = fds[0]; poll.events = 1; poll.revents = 99;
    CHECK(call(POLL, (word)&poll, 1, 0) == 0 && poll.revents == 0);
    CHECK(call(CLOCK, 1, (word)before, 0) == 0);
    CHECK(call(POLL, (word)&poll, 1, 100) == 0 && poll.revents == 0);
    CHECK(call(CLOCK, 1, (word)after, 0) == 0);
    result elapsed = (after[0] - before[0]) * 1000 + (after[1] - before[1]) / 1000000;
    CHECK(elapsed >= 70 && elapsed < 3000);

    poll.fd = fds[1]; poll.events = 4;
    CHECK(call(POLL, (word)&poll, 1, 0) == 1 && poll.revents == 4);
    CHECK(call(WRITE, fds[1], (word)bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(call(POLL, (word)&poll, 1, 0) == 0 && poll.revents == 0);
    poll.fd = fds[0]; poll.events = 1;
    CHECK(call(POLL, (word)&poll, 1, 0) == 1 && poll.revents == 1);
    CHECK(call(READ, fds[0], (word)bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(call(POLL, (word)&poll, 1, 0) == 0 && poll.revents == 0);
    CHECK(call(WRITE, fds[1], (word)"p", 1) == 1);
    CHECK(call(CLOSE, fds[1], 0, 0) == 0);
    CHECK(call(POLL, (word)&poll, 1, 0) == 1 && poll.revents == 17);
    CHECK(call(READ, fds[0], (word)bytes, 1) == 1 && bytes[0] == 'p');
    poll.events = 0;
    CHECK(call(POLL, (word)&poll, 1, 0) == 1 && poll.revents == 16);
    CHECK(call(READ, fds[0], (word)bytes, 1) == 0);
    CHECK(call(CLOSE, fds[0], 0, 0) == 0);

    CHECK(call(PIPE2, (word)fds, 0x80800, 0) == 0);
    CHECK(call(CLOSE, fds[0], 0, 0) == 0);
    poll.fd = fds[1]; poll.events = 4;
    CHECK(call(POLL, (word)&poll, 1, 0) == 1 && poll.revents == 12);
    CHECK(call(CLOSE, fds[1], 0, 0) == 0);

    /* The Python peer writes after seeing this marker. An infinite poll
       must wait for the external write, rather than report an empty pipe. */
    CHECK(call(WRITE, 1, (word)"pipe-wait\n", 10) == 10);
    poll.fd = 0; poll.events = 1;
    CHECK(call(CLOCK, 1, (word)before, 0) == 0);
    CHECK(call(POLL, (word)&poll, 1, (word)-1) == 1 && (poll.revents & 1));
    CHECK(call(CLOCK, 1, (word)after, 0) == 0);
    elapsed = (after[0] - before[0]) * 1000 + (after[1] - before[1]) / 1000000;
    CHECK(elapsed >= 70 && elapsed < 3000);
    CHECK(call(READ, 0, (word)bytes, 1) == 1 && bytes[0] == 'g');
    CHECK(call(WRITE, 1, (word)"pipe-poll-ok\n", 13) == 13);
    call(EXIT, 42, 0, 0);
    __builtin_unreachable();
}

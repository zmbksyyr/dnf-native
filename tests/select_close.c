/* Real glibc pthread close during a blocking select, plus ready-set checks. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ signed_word;
typedef struct { word bits[1024 / (8 * sizeof(word))]; } Bits;
typedef struct { signed_word seconds, fraction; } Time;
typedef struct { unsigned short family, port; unsigned address; char zero[8]; } Address;
extern int socket(int domain, int type, int protocol);
extern int bind(int fd, const void *address, unsigned bytes);
extern int getsockname(int fd, void *address, unsigned *bytes);
extern int listen(int fd, int backlog);
extern signed_word sendto(int fd, const void *bytes, word length, int flags, const void *address, unsigned size);
extern int select(int count, Bits *read, Bits *write, Bits *except, Time *timeout);
extern int close(int fd);
extern int nanosleep(const Time *duration, Time *remaining);
extern int pthread_create(word *thread, const void *attributes, void *(*start)(void *), void *argument);
extern int pthread_join(word thread, void **value);
extern int *__errno_location(void);
extern signed_word write(int fd, const void *bytes, word length);
extern void _exit(int status) __attribute__((noreturn));
#define CHECK(value) do { if (!(value)) _exit(__LINE__); } while (0)

static void add(Bits *set, int fd) { set->bits[fd / (8 * sizeof(word))] |= (word)1 << (fd % (8 * sizeof(word))); }
static int exactly(Bits *set, int fd) {
    Bits expected = {{0}};
    if (fd >= 0) add(&expected, fd);
    for (unsigned i = 0; i < sizeof(set->bits) / sizeof(word); ++i)
        if (set->bits[i] != expected.bits[i]) return 0;
    return 1;
}

static void *closer(void *argument) {
    Time delay = {0, 100000000};
    CHECK(nanosleep(&delay, 0) == 0);
    CHECK(close((int)(word)argument) == 0);
    return 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int fd = socket(2, 2, 0), other = socket(2, 2, 0);
    CHECK(fd >= 0 && other >= 0);
    Address address = {2, 0, 0x0100007f, {0}};
    unsigned size = sizeof(address);
    CHECK(bind(fd, &address, size) == 0 && getsockname(fd, &address, &size) == 0);
    Bits read = {{0}}, writing = {{0}}, except = {{0}};
    add(&read, fd);
    read.bits[0] |= (word)1 << (8 * sizeof(word) - 1); /* Ignored beyond nfds. */
    Time timeout = {0, 30000};
    CHECK(select(other + 1, &read, 0, 0, &timeout) == 0 && exactly(&read, -1));
    CHECK(sendto(other, "r", 1, 0, &address, size) == 1);
    add(&read, fd); add(&writing, other); add(&except, fd);
    timeout.seconds = 1;
    CHECK(select(other + 1, &read, &writing, &except, &timeout) == 2);
    CHECK(exactly(&read, fd) && exactly(&writing, other) && exactly(&except, -1));
    writing = (Bits){{0}}; add(&writing, fd); timeout.seconds = 1;
    CHECK(select(other + 1, &read, &writing, 0, &timeout) == 2);
    CHECK(exactly(&read, fd) && exactly(&writing, fd));
    CHECK(close(fd) == 0 && close(other) == 0);
    timeout.seconds = 0; timeout.fraction = 0;
    CHECK(select(fd + 1, &read, 0, 0, &timeout) == -1 && *__errno_location() == 9);

    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        fd = socket(2, 1, 0);
        address.port = 0;
        CHECK(fd >= 0 && bind(fd, &address, sizeof(address)) == 0 && listen(fd, 1) == 0);
        word thread;
        CHECK(pthread_create(&thread, 0, closer, (void *)(word)fd) == 0);
        read = (Bits){{0}}; add(&read, fd);
        timeout.seconds = 2; timeout.fraction = 0;
        int ready = select(fd + 1, &read, 0, 0, &timeout), error = *__errno_location();
        int report[] = {(int)attempt, ready, error, exactly(&read, -1)};
        CHECK(write(1, report, sizeof(report)) == sizeof(report));
        CHECK(pthread_join(thread, 0) == 0);
        /* Cross-thread close/select is unspecified by Linux. This runtime
           reports EBADF instead of returning readiness for the closed socket. */
        CHECK(ready == -1 && error == 9);
    }
    _exit(42);
}

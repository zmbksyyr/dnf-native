/* Real file identity, truncation and random-device checks through Linux syscalls. */
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

static result call6(word n, word a, word b, word c, word d, word e, word f) {
    result r;
#ifdef __x86_64__
    register word fourth __asm__("r10") = d;
    register word fifth __asm__("r8") = e;
    register word sixth __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c),
                     "r"(fourth), "r"(fifth), "r"(sixth) : "rcx", "r11", "memory");
#else
    register word sixth __asm__("ebp") = f;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c),
                     "S"(d), "D"(e), "r"(sixth) : "memory");
#endif
    return r;
}

#ifdef __x86_64__
enum {READ=0, WRITE=1, OPEN=2, CLOSE=3, STAT=4, FSTAT=5, SEEK=8, FCNTL=72, TRUNCATE=77, EXIT=60,
      FLOCK=73, IOCTL=16, MMAP=9, MUNMAP=11, STAT_BYTES=144, INODE=8, MODE=24, DEVICE=40, SIZE=48};
#else
enum {READ=3, WRITE=4, OPEN=5, CLOSE=6, STAT=195, FSTAT=197, SEEK=19, FCNTL=221, TRUNCATE=194, EXIT=1,
      FLOCK=143, IOCTL=54, MMAP=192, MUNMAP=91, STAT_BYTES=96, INODE=88, MODE=16, DEVICE=32, SIZE=44};
#endif

static void check(int condition, word line) {
    if (!condition) { call(EXIT, line, 0, 0); __builtin_unreachable(); }
}
#define CHECK(c) check((c), __LINE__)

static unsigned long long u64(const unsigned char *p) {
    unsigned long long value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (unsigned long long)p[i] << (8 * i);
    return value;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    unsigned char stat[STAT_BYTES], other[STAT_BYTES], random[128], data[9];
    result fd = call(OPEN, (word)"/sample", 2, 0);
    CHECK(fd >= 0);
    CHECK(call(IOCTL, fd, 0x5401, (word)stat) == -25);
    CHECK(call(FLOCK, fd, 8, 0) == 0);
    CHECK(call(FLOCK, fd, 2, 0) == 0 && call(FLOCK, fd, 2, 0) == 0);
    result contender = call(OPEN, (word)"/sample-link", 2, 0);
    CHECK(contender >= 0 && call(FLOCK, contender, 6, 0) == -11);
    CHECK(call(FLOCK, fd, 8, 0) == 0 && call(FLOCK, fd, 8, 0) == 0);
    CHECK(call(FLOCK, contender, 5, 0) == 0 && call(FLOCK, fd, 5, 0) == 0);
    CHECK(call(FLOCK, contender, 3, 0) == -22);
    CHECK(call(CLOSE, contender, 0, 0) == 0 && call(FLOCK, fd, 8, 0) == 0);
    CHECK(call(FSTAT, fd, (word)stat, 0) == 0);
    CHECK(call(STAT, (word)"/sample-link", (word)other, 0) == 0);
    CHECK(u64(stat + INODE) == u64(other + INODE));
    CHECK(u64(stat + SIZE) == 6);
    CHECK(call(FCNTL, fd, 2, 1) == 0 && call(FCNTL, fd, 1, 0) == 1);
    CHECK(call(SEEK, fd, 3, 0) == 3);
    CHECK(call(TRUNCATE, fd, 4, 0) == 0);
    CHECK(call(SEEK, fd, 0, 1) == 3);
    CHECK(call(TRUNCATE, fd, 9, 0) == 0);
    CHECK(call(SEEK, fd, 0, 1) == 3);
    CHECK(call(SEEK, fd, 0, 0) == 0);
    CHECK(call(READ, fd, (word)data, 9) == 9);
    CHECK(data[0] == 'a' && data[1] == 'b' && data[2] == 'c' && data[3] == 'd');
    for (unsigned i = 4; i < 9; ++i) CHECK(data[i] == 0);
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    fd = call(OPEN, (word)"/sample-link", 0, 0);
    CHECK(fd >= 0 && call(FSTAT, fd, (word)other, 0) == 0);
    CHECK(u64(stat + INODE) == u64(other + INODE) && u64(other + SIZE) == 9);
#ifndef __x86_64__
    if (u64(stat + INODE) <= 0xffffffffULL) CHECK(call(108, fd, (word)other, 0) == 0);
#endif
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)stat, STAT_BYTES) == STAT_BYTES);

    fd = call(OPEN, (word)"/dev/urandom", 0x80800, 0);
    CHECK(fd >= 0);
    CHECK(call(FCNTL, fd, 1, 0) == 1 && call(FCNTL, fd, 3, 0) == 0x800);
    CHECK(call(FCNTL, fd, 4, 0) == 0 && call(FCNTL, fd, 3, 0) == 0);
    CHECK(call(FSTAT, fd, (word)stat, 0) == 0);
    CHECK((stat[MODE + 1] & 0xf0) == 0x20 && u64(stat + DEVICE) == 0x109);
    CHECK(call(STAT, (word)"/dev/urandom", (word)other, 0) == 0);
    CHECK(u64(stat + INODE) == u64(other + INODE));
    CHECK(call(READ, fd, 1, 1) == -14);
    CHECK(call(READ, fd, 0, 0) == 0);
    CHECK(call(READ, fd, (word)random, 64) == 64);
    CHECK(call(READ, fd, (word)(random + 64), 64) == 64);
    unsigned different = 0, nonzero = 0;
    for (unsigned i = 0; i < 64; ++i) { different |= random[i] ^ random[i + 64]; nonzero |= random[i]; }
    CHECK(different && nonzero);
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call(READ, fd, (word)random, 1) == -9);
    CHECK(call(OPEN, (word)"/dev/urandom", 1, 0) == -38);

    fd = call(OPEN, (word)"/shared", 2 | 0x40 | 0x200, 0);
    CHECK(fd >= 0 && call(TRUNCATE, fd, 4096, 0) == 0);
    word shared = (word)call6(MMAP, 0, 4096, 3, 1, (word)fd, 0);
    CHECK(shared && shared < (word)-4095);
    ((volatile unsigned char *)shared)[0] = 's';
    ((volatile unsigned char *)shared)[1] = 'h';
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call6(MUNMAP, shared, 4096, 0, 0, 0, 0) == 0);
    fd = call(OPEN, (word)"/shared", 0, 0);
    CHECK(fd >= 0 && call(READ, fd, (word)data, 2) == 2 && data[0] == 's' && data[1] == 'h');
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)"files-ok\n", 9) == 9);
    call(EXIT, 42, 0, 0);
    __builtin_unreachable();
}

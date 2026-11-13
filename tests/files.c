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
enum {READ=0, WRITE=1, OPEN=2, CLOSE=3, STAT=4, FSTAT=5, SEEK=8, READLINK=89, FCNTL=72, TRUNCATE=77, EXIT=60,
      FLOCK=73, IOCTL=16, MMAP=9, MUNMAP=11, GETDENTS=217, STAT_BYTES=144, INODE=8, MODE=24, DEVICE=40, SIZE=48};
#else
enum {READ=3, WRITE=4, OPEN=5, CLOSE=6, STAT=195, FSTAT=197, SEEK=19, READLINK=85, FCNTL=221, TRUNCATE=194, EXIT=1,
      FLOCK=143, IOCTL=54, MMAP=192, MUNMAP=91, GETDENTS=220, STAT_BYTES=96, INODE=88, MODE=16, DEVICE=32, SIZE=44};
#endif

static void check(int condition, word line) {
    if (!condition) { call(EXIT, line, 0, 0); __builtin_unreachable(); }
}
#define CHECK(c) check((c), __LINE__)

#ifndef FILE_PEER
static unsigned long long u64(const unsigned char *p) {
    unsigned long long value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (unsigned long long)p[i] << (8 * i);
    return value;
}

static int equal(const unsigned char *a, const char *b) {
    while (*a && *a == (unsigned char)*b) { ++a; ++b; }
    return *a == (unsigned char)*b;
}
#endif

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
#ifdef FILE_PEER
    result fd = call(OPEN, (word)"/shared", 2, 0);
    CHECK(fd >= 0);
    word shared = (word)call6(MMAP, 0, 280, 3, 1, (word)fd, 0);
    CHECK(shared && shared < (word)-4095);
    volatile unsigned char *bytes = (volatile unsigned char *)shared;
    CHECK(bytes[0] == 's' && bytes[1] == 'h');
    bytes[2] = 'p';
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call6(MUNMAP, shared, 280, 0, 0, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)"peer-ok\n", 8) == 8);
#else
    unsigned char stat[STAT_BYTES], other[STAT_BYTES], random[128], data[9];
    unsigned char executable[16];
    for (unsigned i = 0; i < sizeof(executable); ++i) executable[i] = 0x7f;
    CHECK(call(READLINK, (word)"/proc/self/exe", (word)executable, sizeof(executable)) == 10);
    CHECK(executable[0] == '/' && executable[1] == 'p' && executable[2] == 'r' && executable[3] == 'o' &&
          executable[4] == 'c' && executable[5] == '-' && executable[6] == 't' && executable[7] == 'e' &&
          executable[8] == 's' && executable[9] == 't' && executable[10] == 0x7f);
    CHECK(call(READLINK, (word)"/proc/self/exe", 1, 0) == 0);
    CHECK(call(READLINK, (word)"/proc/self/exe", 1, 1) == -14);
    result maps = call(OPEN, (word)"/proc/self/maps", 0, 0);
    CHECK(maps >= 0);
    unsigned char map_bytes[512];
    for (unsigned i = 0; i < sizeof(map_bytes); ++i) map_bytes[i] = 0;
    result map_count = call(READ, maps, (word)map_bytes, sizeof(map_bytes) - 1);
    CHECK(map_count > 0 && map_count < (result)sizeof(map_bytes));
    unsigned map_path = 0;
    for (result i = 0; i + 10 <= map_count; ++i) {
        unsigned match = map_bytes[i] == '/';
        for (unsigned j = 0; match && j < 10; ++j)
            match = map_bytes[i + j] == (unsigned char)"/proc-test"[j];
        if (match) map_path = 1;
    }
    CHECK(map_path && call(CLOSE, maps, 0, 0) == 0);

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

    fd = call(OPEN, (word)"/shared", 2 | 0x40 | 0x200 | 0x20000, 0);
    CHECK(fd >= 0 && call(TRUNCATE, fd, 280, 0) == 0);
    word shared = (word)call6(MMAP, 0, 280, 3, 1, (word)fd, 0);
    CHECK(shared && shared < (word)-4095);
    ((volatile unsigned char *)shared)[0] = 's';
    ((volatile unsigned char *)shared)[1] = 'h';
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    fd = call(OPEN, (word)"/shared", 0, 0);
    CHECK(fd >= 0);
    word alias = (word)call6(MMAP, 0, 280, 1, 1, (word)fd, 0);
    CHECK(alias && alias < (word)-4095 && ((volatile unsigned char *)alias)[1] == 'h');
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)"shared-ready\n", 13) == 13);
    CHECK(call(READ, 0, (word)data, 1) == 1 && data[0] == 'g');
    CHECK(((volatile unsigned char *)shared)[2] == 'p' && ((volatile unsigned char *)alias)[2] == 'p');
    CHECK(call6(MUNMAP, shared, 280, 0, 0, 0, 0) == 0);
    CHECK(((volatile unsigned char *)alias)[2] == 'p');
    CHECK(call6(MUNMAP, alias, 280, 0, 0, 0, 0) == 0);
    fd = call(OPEN, (word)"/shared", 0, 0);
    CHECK(fd >= 0 && call(READ, fd, (word)data, 2) == 2 && data[0] == 's' && data[1] == 'h');
    CHECK(call(CLOSE, fd, 0, 0) == 0);

    /* 32-byte reads force pagination; short reads must not mean EOF. */
    fd = call(OPEN, (word)"/directory", 0x10000, 0);
    CHECK(fd >= 0);
    unsigned char entries[32];
    CHECK(call(GETDENTS, fd, (word)entries, 1) == -22);
    unsigned seen = 0, records = 0;
    for (unsigned batch = 0; batch < 10; ++batch) {
        result bytes = call(GETDENTS, fd, (word)entries, sizeof(entries));
        CHECK(bytes >= 0 && bytes <= (result)sizeof(entries));
        if (!bytes) break;
        unsigned length = entries[16] | (unsigned)entries[17] << 8;
        CHECK(length == (unsigned)bytes && !(length & 7));
        ++records;
        CHECK(u64(entries + 8) == records);
        unsigned bit;
        const char *path;
        if (equal(entries + 19, ".")) { bit = 1; path = "/directory"; }
        else if (equal(entries + 19, "..")) { bit = 2; path = "/"; }
        else if (equal(entries + 19, "alpha")) { bit = 4; path = "/directory/alpha"; }
        else { CHECK(equal(entries + 19, "beta")); bit = 8; path = "/directory/beta"; }
        CHECK(!(seen & bit)); seen |= bit;
        CHECK(entries[18] == (bit == 4 ? 8 : 4));
        CHECK(call(STAT, (word)path, (word)other, 0) == 0 && u64(entries) != 0);
    }
    CHECK(seen == 15 && records == 4 && call(GETDENTS, fd, (word)entries, sizeof(entries)) == 0);
    CHECK(call(SEEK, fd, 0, 0) == 0 && call(GETDENTS, fd, (word)entries, sizeof(entries)) > 0);
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    fd = call(OPEN, (word)"/directory", 0x10000, 0);
    CHECK(fd >= 0 && call(GETDENTS, fd, (word)entries, sizeof(entries)) > 0);
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    fd = call(OPEN, (word)"/sample", 0, 0);
    CHECK(fd >= 0 && call(GETDENTS, fd, (word)entries, sizeof(entries)) == -20);
    CHECK(call(CLOSE, fd, 0, 0) == 0);
    CHECK(call(WRITE, 1, (word)"files-ok\n", 9) == 9);
#endif
    call(EXIT, 42, 0, 0);
    __builtin_unreachable();
}

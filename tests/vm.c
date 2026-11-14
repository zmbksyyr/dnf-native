/* Guest-visible anonymous VM behavior; no libc or host implementation hooks. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

static __attribute__((noinline)) result call6(word n, word a, word b, word c, word d, word e, word f) {
    result value;
#ifdef __x86_64__
    register word fourth __asm__("r10") = d;
    register word fifth __asm__("r8") = e;
    register word sixth __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(value) : "a"(n), "D"(a), "S"(b), "d"(c),
                     "r"(fourth), "r"(fifth), "r"(sixth) : "rcx", "r11", "memory");
#else
    register word sixth __asm__("ebp") = f;
    __asm__ volatile("int $0x80" : "=a"(value) : "a"(n), "b"(a), "c"(b), "d"(c),
                     "S"(d), "D"(e), "r"(sixth) : "memory");
#endif
    return value;
}

#ifdef __x86_64__
enum {WRITE = 1, MMAP = 9, MPROTECT = 10, MUNMAP = 11, EXIT = 60};
#else
enum {WRITE = 4, MMAP = 192, MPROTECT = 125, MUNMAP = 91, MREMAP = 163, EXIT = 1};
#endif

static void check(int condition, word line) {
    if (!condition) {
        call6(EXIT, line, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
}
#define CHECK(c) check((c), __LINE__)
#define PROTECT(address, size, flags) call6(MPROTECT, (address), (size), (flags), 0, 0, 0)
#define UNMAP(address, size) call6(MUNMAP, (address), (size), 0, 0, 0, 0)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* Seventeen pages cross both a bitmap byte and the Windows 64 KiB unit. */
    const word page = 4096, bytes = 17 * 4096;
    word base = (word)call6(MMAP, 0, bytes, 0, 0x4022, (word)-1, 0);
    CHECK(base && base < (word)-4095 && !(base & (page - 1)));
    word middle = base + 8 * page, last = base + 16 * page;
    CHECK(PROTECT(base, bytes, 0) == 0);
    CHECK(PROTECT(middle, page, 3) == 0);
    volatile unsigned char *data = (volatile unsigned char *)middle;
    CHECK(data[0] == 0);
    data[0] = 0x5a;
    CHECK(PROTECT(base, bytes, 0) == 0);
    CHECK(PROTECT(middle, page, 3) == 0);
    CHECK(data[0] == 0x5a);

    CHECK(UNMAP(last, page) == 0);
    CHECK(PROTECT(last, page, 0) == -12);
    CHECK(PROTECT(last, page, 3) == -12);
    CHECK(PROTECT(base, bytes, 0) == -12);
    CHECK(data[0] == 0x5a); /* A rejected range must not protect earlier pages. */
    CHECK(UNMAP(middle, page) == 0);
    CHECK(PROTECT(middle, page, 3) == -12);

    CHECK((word)call6(MMAP, middle, page, 3, 0x32, (word)-1, 0) == middle);
    CHECK(data[0] == 0);
    data[0] = 0x27;
    CHECK(PROTECT(base, bytes, 3) == -12);
    CHECK((word)call6(MMAP, last, page, 0, 0x32, (word)-1, 0) == last);
    CHECK(PROTECT(last, page, 0) == 0);
    CHECK(PROTECT(base, bytes, 3) == 0);
    CHECK(data[0] == 0x27);
    CHECK(*(volatile unsigned char *)base == 0);
    CHECK(*(volatile unsigned char *)last == 0);
    CHECK(PROTECT(base, bytes, 0) == 0);
    CHECK(PROTECT(base, bytes, 3) == 0);
    CHECK(data[0] == 0x27);
    CHECK(UNMAP(base, bytes) == 0);
    CHECK(PROTECT(base, bytes, 3) == -12);

    /* Allocators trim alignment head/tail pages and later unmap the middle.
       Releasing every page in separate calls must also release the reservation. */
    for (unsigned i = 0; i < 1500; ++i) {
        word trimmed = (word)call6(MMAP, 0, bytes, 3, 0x22, (word)-1, 0);
        CHECK(trimmed && trimmed < (word)-4095);
        *(volatile unsigned char *)(trimmed + 8 * page) = (unsigned char)i;
        CHECK(UNMAP(trimmed, 8 * page) == 0);
        CHECK(UNMAP(trimmed + 16 * page, page) == 0);
        CHECK(*(volatile unsigned char *)(trimmed + 8 * page) == (unsigned char)i);
        CHECK(UNMAP(trimmed + 8 * page, 8 * page) == 0);
    }

#ifndef __x86_64__
    word remap = (word)call6(MMAP, 0, page, 3, 0x22, (word)-1, 0);
    CHECK(remap && remap < (word)-4095);
    *(volatile unsigned char *)remap = 0x6b;
    word grown = (word)call6(MREMAP, remap, page, 2 * page, 1, 0, 0);
    CHECK(grown && grown < (word)-4095 && ((volatile unsigned char *)grown)[0] == 0x6b);
    CHECK(UNMAP(grown, 2 * page) == 0);

    word code = (word)call6(MMAP, 0, page, 3, 0x22, (word)-1, 0);
    CHECK(code && code < (word)-4095);
    unsigned char *instructions = (unsigned char *)code;
    instructions[0] = 0xb8; instructions[1] = 42; instructions[2] = 0;
    instructions[3] = 0; instructions[4] = 0; instructions[5] = 0xc3;
    CHECK(PROTECT(code, page, 5) == 0);
    CHECK(((int (*)(void))code)() == 42);
    CHECK(UNMAP(code, page) == 0);
#endif

    const char passed[] = "anon-vm-ok\n";
    CHECK(call6(WRITE, 1, (word)passed, sizeof(passed) - 1, 0, 0, 0) == (result)(sizeof(passed) - 1));
    call6(EXIT, 42, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

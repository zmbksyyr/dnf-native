/* File replacement and explicit open-target limits; msync survives closing fd. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
static __attribute__((noinline)) result call6(word n, word a, word b, word c, word d, word e, word f) {
    result value;
#ifdef __x86_64__
    register word fourth __asm__("r10") = d;
    register word fifth __asm__("r8") = e;
    register word sixth __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(value) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(fourth), "r"(fifth), "r"(sixth)
                     : "rcx", "r11", "memory");
#else
    register word sixth __asm__("ebp") = f;
    __asm__ volatile("int $0x80" : "=a"(value) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "r"(sixth) : "memory");
#endif
    return value;
}
#ifdef __x86_64__
enum {READ=0, WRITE=1, OPEN=2, CLOSE=3, TRUNCATE=77, RENAME=82, MMAP=9, MUNMAP=11, MSYNC=26, EXIT=60};
#else
enum {READ=3, WRITE=4, OPEN=5, CLOSE=6, TRUNCATE=194, RENAME=38, MMAP=192, MUNMAP=91, MSYNC=144, EXIT=1};
#endif
#define CALL(n,a,b,c) call6((n),(word)(a),(word)(b),(word)(c),0,0,0)
#define CHECK(value) do { if (!(value)) { CALL(EXIT,__LINE__,0,0); __builtin_unreachable(); } } while (0)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
#ifdef NR_RENAME
    result source = CALL(OPEN,"/incoming",0x242,0600), target = CALL(OPEN,"/destination",0x242,0600);
    CHECK(source >= 0 && target >= 0);
    CHECK(CALL(WRITE,source,"n",1) == 1 && CALL(WRITE,target,"o",1) == 1);
    CHECK(CALL(CLOSE,target,0,0) == 0);
    target = CALL(OPEN,"/destination",0,0);
    CHECK(target >= 0);
    /* Windows 7 MoveFileEx cannot replace an open destination. Keep both
       files intact and report the limitation instead of deleting it first. */
    CHECK(CALL(RENAME,"/incoming","/destination",0) == -13);
    char value;
    CHECK(CALL(READ,target,&value,1) == 1 && value == 'o');
    CHECK(CALL(CLOSE,target,0,0) == 0);
    CHECK(CALL(RENAME,"/incoming","/destination",0) == 0);
    CHECK(CALL(CLOSE,source,0,0) == 0);
    target = CALL(OPEN,"/destination",0,0);
    CHECK(target >= 0 && CALL(READ,target,&value,1) == 1 && value == 'n');
    CHECK(CALL(CLOSE,target,0,0) == 0);
    CHECK(CALL(RENAME,"/destination","/destination",0) == 0);
    CHECK(CALL(RENAME,"/missing","/destination",0) == -2);
    CHECK(CALL(RENAME,"/destination",1,0) == -14);
#else
    result fd = CALL(OPEN,"/shared",0x242,0600);
    CHECK(fd >= 0 && CALL(TRUNCATE,fd,8192,0) == 0);
    word base = (word)call6(MMAP,0,8192,3,1,(word)fd,0);
    CHECK(base && base < (word)-4095);
    volatile unsigned char *bytes = (volatile unsigned char *)base;
    bytes[0] = 's'; bytes[8191] = 'z';
    CHECK(CALL(CLOSE,fd,0,0) == 0);
    CHECK(CALL(MSYNC,base,8192,4) == 0);
    CHECK(CALL(MSYNC,base+4096,4096,4) == 0);
    CHECK(CALL(MSYNC,base,8192,1) == 0 && CALL(MSYNC,base,8192,0) == 0);
    CHECK(CALL(MSYNC,base,0,4) == 0);
    CHECK(CALL(MSYNC,base+1,8191,4) == -22);
    CHECK(CALL(MSYNC,base,8192,5) == -22 && CALL(MSYNC,base,8192,8) == -22);
    CHECK(CALL(MUNMAP,base,8192,0) == 0 && CALL(MSYNC,base,8192,4) == -12);
    base = (word)call6(MMAP,0,4096,3,0x22,(word)-1,0);
    CHECK(base && base < (word)-4095 && CALL(MSYNC,base,4096,4) == 0);
    CHECK(CALL(MUNMAP,base,4096,0) == 0);
#endif
    CALL(EXIT,42,0,0);
    __builtin_unreachable();
}

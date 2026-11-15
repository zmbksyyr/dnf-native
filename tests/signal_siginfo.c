/* Install and deliver SA_SIGINFO through the guest signal frame. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {WRITE=1, GETPID=39, GETTID=186, KILL=62, TGKILL=234, EXIT=60, ACTION=13, MASK=14, MACHINE=40, MASK_OFFSET=296, RESULT_REG=13, PC_REG=16, SP_REG=15};
#else
enum {WRITE=4, GETPID=20, GETTID=224, KILL=37, TGKILL=270, EXIT=1, ACTION=174, MASK=175, MACHINE=20, MASK_OFFSET=108, RESULT_REG=11, PC_REG=14, SP_REG=7};
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
#define CHECK(c) do { if (!(c)) { call(EXIT, __LINE__, 0, 0, 0); __builtin_unreachable(); } } while (0)
static volatile word observed;
static word expected_pid;
#ifndef NR_GLIBC
static int expected_origin;
static unsigned nodefer_mode, nested_calls, depth, max_depth;
static void nested_handler(int signal, void *info, void *context) {
    CHECK(signal==12 && info && context);
    unsigned long long mask=0;
    CHECK(call(MASK,0,0,(word)&mask,8)==0);
    CHECK(((mask>>11)&1)==(nodefer_mode!=1));
    ++depth;
    if (depth>max_depth) max_depth=depth;
    ++nested_calls;
    if (nested_calls==1) {
        CHECK(call(KILL,expected_pid,12,0,0)==0);
        CHECK(nested_calls==(nodefer_mode==1 ? 2u : 1u));
    }
    --depth;
}
static void handler(int signal, void *info, void *context) {
    CHECK(signal == 12 && info && context);
    const int *si=(const int *)info;
    CHECK(si[0] == 12 && si[1] == 0 && si[2] == expected_origin);
    const int *sender=si+(sizeof(word)==8 ? 4 : 3);
    CHECK((word)sender[0] == expected_pid && sender[1] == 1000);
    word *regs=(word *)((unsigned char *)context+MACHINE);
    CHECK(regs[RESULT_REG] == 0 && regs[PC_REG] && regs[SP_REG]);
    unsigned long long *saved_mask=(unsigned long long *)((unsigned char *)context+MASK_OFFSET);
    CHECK(*saved_mask == (1ULL<<9));
    unsigned long long current=0;
    CHECK(call(MASK,0,0,(word)&current,8)==0);
    CHECK((current & ((1ULL<<9)|(1ULL<<11)|(1ULL<<14))) == ((1ULL<<9)|(1ULL<<11)|(1ULL<<14)));
    regs[RESULT_REG]=77;
    *saved_mask|=1ULL<<13;
    ++observed;
}

__attribute__((naked)) static void restore(void) {
#ifdef __x86_64__
    __asm__ volatile("mov $15, %eax; syscall; ud2");
#else
    __asm__ volatile("mov $173, %eax; int $0x80; ud2");
#endif
}
#endif
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
#ifdef NR_GLIBC
    extern int __libc_start_main(int (*)(void), int, char **, void *, void *, void *, void *);
    extern int glibc_main(void);
    __libc_start_main(glibc_main,0,0,0,0,0,0);
    __builtin_unreachable();
#else
    struct { word handler, flags, restorer; unsigned long long mask; } action = {
        (word)handler, 0x10000004u, 0, 1ULL<<14
    }, old={0};
    expected_pid=(word)call(GETPID,0,0,0,0);
    unsigned long long mask=1ULL<<9, current=0;
    CHECK(call(MASK,2,(word)&mask,0,8)==0);
    CHECK(call(ACTION, 12, (word)&action, 0, 7) == -22);
    CHECK(call(ACTION, 12, 1, 0, 8) == -14);
    action.flags |= 0x08000000;
    CHECK(call(ACTION, 12, (word)&action, 0, 8) == -38);
    action.flags &= ~0x08000000u;
    CHECK(call(ACTION, 12, (word)&action, 0, 8) == 0);
    CHECK(call(ACTION, 12, 0, (word)&old, 8) == 0);
    CHECK(old.handler==action.handler && old.flags==action.flags && old.mask==action.mask);
    CHECK(call(KILL, expected_pid, 12, 0, 0) == 77);
    CHECK(observed == 1);
    CHECK(call(MASK,0,0,(word)&current,8)==0 && current==(mask|(1ULL<<13)));
    CHECK(call(MASK,2,(word)&mask,0,8)==0);
    action.flags |= 0x04000000u;
    action.restorer=(word)restore;
    CHECK(call(ACTION, 12, (word)&action, 0, 8) == 0);
    expected_origin=-6;
    CHECK(call(TGKILL,expected_pid,(word)call(GETTID,0,0,0,0),12,0)==77);
    CHECK(observed==2);
    /* Exact Frida flags: SIGINFO|RESTORER|NODEFER. Also verify that an explicit
       action mask still defers the signal, and that each return restores it. */
    mask=0;
    CHECK(call(MASK,2,(word)&mask,0,8)==0);
    action.handler=(word)nested_handler;
    for (nodefer_mode=0; nodefer_mode<3; ++nodefer_mode) {
        action.flags=0x04000004u | (nodefer_mode ? 0x40000000u : 0);
        action.mask=nodefer_mode==2 ? 1ULL<<11 : 0;
        depth=max_depth=nested_calls=0;
        CHECK(call(ACTION,12,(word)&action,0,8)==0);
        CHECK(call(KILL,expected_pid,12,0,0)==0);
        CHECK(nested_calls==2 && depth==0 && max_depth==(nodefer_mode==1 ? 2u : 1u));
        CHECK(call(MASK,0,0,(word)&current,8)==0 && current==0);
    }
    const char message[]="native-siginfo-ok\n";
    CHECK(call(WRITE, 1, (word)message, sizeof(message)-1, 0) == sizeof(message)-1);
    call(EXIT, 42, 0, 0, 0);
    __builtin_unreachable();
#endif
}

#ifdef NR_GLIBC
extern int sigaction(int, const void *, void *);
extern int raise(int);
static void glibc_handler(int signal, void *info, void *context) {
    CHECK(signal==12 && info && context);
    int *fields=(int *)info;
    CHECK(fields[0]==12 && fields[2]==-6);
    const int *sender=fields+(sizeof(word)==8 ? 4 : 3);
    CHECK((word)sender[0]==expected_pid);
    word *regs=(word *)((unsigned char *)context+MACHINE);
    CHECK(regs[PC_REG] && regs[SP_REG]);
    ++observed;
}
int glibc_main(void) {
    struct {
        word handler;
        unsigned char mask[128];
        unsigned flags;
        word restorer;
    } action={0}, old={0};
    action.handler=(word)glibc_handler; action.flags=0x10000004;
    expected_pid=(word)call(GETPID,0,0,0,0);
    CHECK(sigaction(12,&action,&old)==0);
    CHECK(raise(12)==0 && observed==1);
    CHECK(sigaction(12,0,&old)==0 && old.handler==action.handler && (old.flags&4));
    CHECK(raise(12)==0 && observed==2);
    const char text[]="native-glibc-siginfo-ok\n";
    CHECK(call(WRITE,1,(word)text,sizeof(text)-1,0)==sizeof(text)-1);
    return 42;
}
#endif

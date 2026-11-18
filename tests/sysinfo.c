/* Original glibc's i386 GS:16 gateway: register/flags/FP preservation and
   signal/tracing fallbacks. Build with the pinned, unmodified glibc loader. */
typedef __UINTPTR_TYPE__ word;
extern int __libc_start_main(int (*)(void), int, char **, void *, void *, void *, void *);
extern int pthread_create(word *, const void *, void *(*)(void *), void *);
extern int pthread_join(word, void **);
extern int memcmp(const void *, const void *, unsigned);
extern void _exit(int) __attribute__((noreturn));
#define CHECK(c) do { if (!(c)) _exit(100 + __LINE__ % 100); } while (0)

static unsigned char original_fp[512] __attribute__((aligned(16),used));
static unsigned char before_fp[512] __attribute__((aligned(16),used));
static unsigned char after_fp[512] __attribute__((aligned(16),used));
static const unsigned vector[4] __attribute__((aligned(16),used)) = {0x12345678,0x9abcdef0,0x13579bdf,0x2468ace0};
static const unsigned mxcsr __attribute__((used)) = 0x5f80;
static unsigned before_flags __attribute__((used)),after_flags __attribute__((used));
static unsigned before_sp __attribute__((used)),before_bp __attribute__((used)),bad __attribute__((used));
static volatile unsigned seen;

static int raw(unsigned number, unsigned a, unsigned b, unsigned d, unsigned s) {
    int result;
    __asm__ volatile("int $0x80":"=a"(result):"a"(number),"b"(a),"c"(b),"d"(d),"S"(s):"memory");
    return result;
}
static unsigned query(unsigned number, unsigned argument) {
    unsigned result;
    bad=0;
    __asm__ volatile(
        "fxsave original_fp\n\tfninit\n\tfldpi\n\tfld1\n\tldmxcsr mxcsr\n\t"
        "movdqa vector, %%xmm0\n\tmovdqa vector, %%xmm1\n\tmovdqa vector, %%xmm2\n\t"
        "movdqa vector, %%xmm3\n\tmovdqa vector, %%xmm4\n\tmovdqa vector, %%xmm5\n\t"
        "movdqa vector, %%xmm6\n\tmovdqa vector, %%xmm7\n\tfxsave before_fp\n\t"
        "movl $0x12345678, %%ecx\n\tmovl $0x23456789, %%edx\n\t"
        "movl $0x3456789a, %%esi\n\tmovl $0x456789ab, %%edi\n\t"
        "movl %%esp, before_sp\n\tmovl %%ebp, before_bp\n\t"
        "pushl $0xcd7\n\tpopfl\n\tpushfl\n\tpopl before_flags\n\t"
        "call *%%gs:16\n\tpushfl\n\tpopl after_flags\n\tcld\n\tfxsave after_fp\n\t"
        "cmpl $0x12345678, %%ecx\n\tjne 1f\n\tcmpl $0x23456789, %%edx\n\tjne 1f\n\t"
        "cmpl $0x3456789a, %%esi\n\tjne 1f\n\tcmpl $0x456789ab, %%edi\n\tjne 1f\n\t"
        "cmpl %%esp, before_sp\n\tjne 1f\n\tcmpl %%ebp, before_bp\n\tjne 1f\n\tjmp 2f\n\t"
        "1: movl $1, bad\n\t2: fxrstor original_fp"
        : "=a"(result), "+b"(argument) : "a"(number)
        : "ecx","edx","esi","edi","memory","cc");
    CHECK(!bad && !((before_flags ^ after_flags) & 0xcd5));
    CHECK(memcmp(before_fp,after_fp,sizeof(before_fp))==0);
    CHECK(argument==0);
    return result;
}
__attribute__((naked)) static void custom_target(void) {
    __asm__ volatile("movl $0x654321, %eax; ret");
}
__attribute__((naked)) static void restore(void) { __asm__ volatile("movl $173, %eax; int $0x80"); }
static void handler(int signal, void *info, void *context) {
    (void)info; (void)context; CHECK(signal==12);++seen;
}
static void *worker(void *unused) {
    (void)unused;
    CHECK(query(224,0)==(unsigned)raw(224,0,0,0,0));
    CHECK(query(20,0)==(unsigned)raw(20,0,0,0,0));
    return 0;
}
static int main_test(void) {
    for (unsigned i=0;i<1000;++i) {
        CHECK(query(20,0)==(unsigned)raw(20,0,0,0,0));
        CHECK(query(224,0)==(unsigned)raw(224,0,0,0,0));
        unsigned before=(unsigned)raw(13,0,0,0,0),actual=query(13,0),after=(unsigned)raw(13,0,0,0,0);
        CHECK(before<=actual && actual<=after);
    }
    /* A mutable TLS callback is not the sysinfo gateway, even for a known nr. */
    word target;
    __asm__ volatile("movl %%gs:16, %0":"=r"(target));
    __asm__ volatile("movl %0, %%gs:16"::"r"(custom_target):"memory");
    CHECK(query(20,0)==0x654321);
    __asm__ volatile("movl %0, %%gs:16"::"r"(target):"memory");
    /* The pinned loader entry may itself be detoured by instrumentation.
       Change only this process's mapped copy, verify all six original bytes,
       then restore both the bytes and executable/read-only protection. Linux
       normally uses its kernel vDSO instead of this original-loader entry. */
    unsigned char *gateway=(unsigned char *)target;
    if (gateway[0]==0xcd && gateway[1]==0x80 && gateway[2]==0xc3) {
        const unsigned char original[6]={0xcd,0x80,0xc3,0x8d,0xb6,0x00};
        CHECK(memcmp(gateway,original,sizeof(original))==0);
        CHECK(raw(125,target & ~4095u,4096,7,0)==0);
        const unsigned char replacement[6]={0xb8,0x21,0x43,0x65,0x00,0xc3};
        for (unsigned i=0;i<sizeof(replacement);++i) gateway[i]=replacement[i];
        CHECK(query(20,0)==0x654321);
        for (unsigned i=0;i<sizeof(original);++i) gateway[i]=original[i];
        CHECK(raw(125,target & ~4095u,4096,5,0)==0);
        CHECK(memcmp(gateway,original,sizeof(original))==0);
        CHECK(query(20,0)==(unsigned)raw(20,0,0,0,0));
    }
    /* Non-NULL time and invalid pointers must keep the original error path. */
    unsigned t=0,result;
    __asm__ volatile("call *%%gs:16":"=a"(result):"a"(13),"b"(&t):"memory","cc");
    CHECK(result==t && t>0);
    __asm__ volatile("call *%%gs:16":"=a"(result):"a"(13),"b"(1):"memory","cc");
    CHECK((int)result==-14);
    /* A self-directed blocked signal stays pending across the fast candidates,
       then runs when unblocked; delivery must never disappear in a shortcut. */
    struct { word handler,flags,restorer; unsigned long long mask; } action={(word)handler,0x04000004,(word)restore,0};
    unsigned long long mask=1ULL<<11;
    CHECK(raw(174,12,(word)&action,0,8)==0);
    CHECK(raw(175,0,(word)&mask,0,8)==0);
    CHECK(raw(37,query(20,0),12,0,0)==0);
    CHECK(query(13,0)>0 && seen==0);
    CHECK(raw(175,1,(word)&mask,0,8)==0 && seen==1);
    const char ready[]="sysinfo-signal-ready\n";
    CHECK(raw(4,1,(word)ready,sizeof(ready)-1,0)==sizeof(ready)-1);
    unsigned deadline=query(13,0)+5;
    while (seen==1 && query(13,0)<deadline) CHECK(query(20,0)>0);
    CHECK(seen==2);
    word thread;
    CHECK(pthread_create(&thread,0,worker,0)==0);
    CHECK(pthread_join(thread,0)==0);
    const char message[]="native-sysinfo-state-ok\n";
    CHECK(raw(4,1,(word)message,sizeof(message)-1,0)==sizeof(message)-1);
    return 42;
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    __libc_start_main(main_test,0,0,0,0,0,0);
    __builtin_unreachable();
}

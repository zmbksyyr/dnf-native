/* Delayed TCP reads must retain byte order and level-triggered readiness. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {READ=0, WRITE=1, CLOSE=3, POLL=7, SELECT=23, SLEEP=35, CREATE=291, CTL=233, WAIT=232, EXIT=60};
#else
enum {READ=3, WRITE=4, CLOSE=6, POLL=168, SELECT=142, SLEEP=162, CREATE=329, CTL=255, WAIT=256, EXIT=1};
#endif
static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word extra __asm__("r10")=d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(extra) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
    return r;
}
static result net(unsigned op, word a, word b, word c, word d, word e) {
#ifdef __x86_64__
    static const unsigned numbers[]={0,41,49,42,50,43,0,0,0,0,0,0,0,0,54,55};
    register word extra __asm__("r8")=e, fourth __asm__("r10")=d;
    result r;
    __asm__ volatile("syscall" : "=a"(r) : "a"((word)numbers[op]), "D"(a), "S"(b), "d"(c), "r"(fourth), "r"(extra) : "rcx", "r11", "memory");
    return r;
#else
    word args[]={a,b,c,d,e};
    return call(102,op,(word)args,0,0);
#endif
}
static unsigned total;
static result observed;
#define CHECK(c) do { if (!(c)) { int report[]={__LINE__,(int)observed,(int)total}; call(WRITE,1,(word)report,sizeof(report),0); call(EXIT,1,0,0,0); __builtin_unreachable(); } } while (0)
struct Event {unsigned events; unsigned long long data;} __attribute__((packed));
static void readable(int fd, int epoll) {
    struct {int fd; short events,revents;} poll={fd,1,0};
    observed=call(POLL,(word)&poll,1,5000,0);
    CHECK(observed==1 && (poll.revents&1));
    word bits[1024/(8*sizeof(word))]={0};
    bits[fd/(8*sizeof(word))]|=(word)1<<(fd%(8*sizeof(word)));
    result timeout[2]={0,0};
    result selected;
#ifdef __x86_64__
    register word fifth __asm__("r8")=(word)timeout, fourth __asm__("r10")=0;
    __asm__ volatile("syscall" : "=a"(selected) : "a"((word)SELECT), "D"((word)(fd+1)), "S"((word)bits), "d"((word)0), "r"(fourth), "r"(fifth) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(selected) : "a"((word)SELECT), "b"((word)(fd+1)), "c"((word)bits), "d"((word)0), "S"((word)0), "D"((word)timeout) : "memory");
#endif
    observed=selected;
    CHECK(observed==1 && (bits[fd/(8*sizeof(word))]&((word)1<<(fd%(8*sizeof(word))))));
    struct Event event={0};
    observed=call(WAIT,epoll,(word)&event,1,0);
    CHECK(observed==1 && (event.events&1) && event.data==0x123456789abcdef0ULL);
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    result fd=net(1,2,1,0,0,0);
    CHECK(fd>=0);
    if (NR_CASE==3) {
        int capacity=262144;
        CHECK(net(14,fd,1,8,(word)&capacity,4)==0);
    }
    unsigned char address[16]={2,0,(NR_PORT>>8)&255,NR_PORT&255,127,0,0,1};
    if (NR_CASE==4) {
        CHECK(net(2,fd,(word)address,16,0,0)==0 && net(4,fd,1,0,0,0)==0);
        CHECK(call(WRITE,1,(word)"L",1,0)==1);
        result accepted=net(5,fd,0,0,0,0);
        CHECK(accepted>=0 && call(CLOSE,fd,0,0,0)==0);
        fd=accepted;
    } else CHECK(net(3,fd,(word)address,16,0,0)==0);
    result epoll=call(CREATE,0,0,0,0);
    struct Event event={1,0x123456789abcdef0ULL};
    CHECK(epoll>=0 && call(CTL,epoll,1,fd,(word)&event)==0);
    CHECK(call(WRITE,fd,(word)"x",1,0)==1);
    result delay[2]={0,250000000};
    CHECK(call(SLEEP,(word)delay,0,0,0)==0);
    if (NR_CASE==0 || NR_CASE==1 || NR_CASE==4) {
        int error=0,length=4;
        observed=net(15,fd,1,4,(word)&error,(word)&length);
        CHECK(observed==0 && error==104);
    }
    unsigned target=NR_CASE==1 ? 49152 : NR_CASE==2 ? 262144 : NR_CASE==3 ? 131072 : 4096;
    while (total<target) {
        readable((int)fd,(int)epoll);
        unsigned char buffer[1021];
        observed=call(READ,fd,(word)buffer,sizeof(buffer),0);
        CHECK(observed>0 && (word)observed<=sizeof(buffer));
        for (unsigned i=0;i<(unsigned)observed;++i) CHECK(buffer[i]==(unsigned char)((total+i)%251));
        total+=(unsigned)observed;
    }
    CHECK(total==target);
    readable((int)fd,(int)epoll);
    char byte;
    observed=call(READ,fd,(word)&byte,1,0);
    CHECK(observed==(NR_CASE==3 ? -104 : 0));
    CHECK(call(READ,fd,(word)&byte,1,0)==0);
    CHECK(call(CLOSE,fd,0,0,0)==0 && call(CLOSE,epoll,0,0,0)==0);
    CHECK(call(WRITE,1,(word)"buffered-ok\n",12,0)==12);
    call(EXIT,42,0,0,0); __builtin_unreachable();
}

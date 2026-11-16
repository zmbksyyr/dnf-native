/* Linux TCP EOF/reset readiness contract; own fixture, no libc. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
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
#ifdef __x86_64__
enum {READ=0, WRITE=1, CLOSE=3, POLL=7, CREATE=291, CTL=233, WAIT=232, EXIT=60};
#else
enum {READ=3, WRITE=4, CLOSE=6, POLL=168, CREATE=329, CTL=255, WAIT=256, EXIT=1};
#endif
#define CHECK(c) do { if (!(c)) { call(EXIT, 100+__LINE__, 0, 0, 0); __builtin_unreachable(); } } while (0)
static result net(unsigned op, word a, word b, word c) {
#ifdef __x86_64__
    return call(op==1 ? 41 : 42,a,b,c,0);
#else
    word args[]={a,b,c}; return call(102,op,(word)args,0,0);
#endif
}
struct Event { unsigned events; unsigned long long data; } __attribute__((packed));
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct {unsigned short family,port; unsigned address; unsigned char zero[8];} address=
        {2,(NR_PORT>>8)|((NR_PORT&255)<<8),0x0100007f,{0}};
    result socket=net(1,2,1,0);CHECK(socket>=0);
    CHECK(net(3,socket,(word)&address,sizeof(address))==0);
    result epoll=call(CREATE,0,0,0,0);CHECK(epoll>=0);
    struct Event input={1,0x123456789abcdefULL},output={0,0};
    CHECK(call(CTL,epoll,1,socket,(word)&input)==0);
    CHECK(call(WRITE,socket,(word)"x",1,0)==1);
    struct {int fd; short events,revents;} poll={(int)socket,1,0};
    CHECK(call(POLL,(word)&poll,1,2000,0)==1);
    CHECK(call(WAIT,epoll,(word)&output,1,2000)==1);
    int observed[]={poll.revents,(int)output.events};
    CHECK(call(WRITE,1,(word)observed,sizeof(observed),0)==sizeof(observed));
    CHECK((poll.revents&1) && (output.events&1) && output.data==input.data);
    char byte;result read=call(READ,socket,(word)&byte,1,0);
    CHECK(read==(NR_RESET ? -104 : 0));
    CHECK(call(CLOSE,socket,0,0,0)==0);
    CHECK(call(CLOSE,epoll,0,0,0)==0);
    call(EXIT,42,0,0,0);__builtin_unreachable();
}

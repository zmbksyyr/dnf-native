/* Per-call nonblocking I/O must preserve shared mode, other I/O and byte counts. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {READ=0,WRITE=1,CLOSE=3,POLL=7,FCNTL=72,SLEEP=35,EXIT=60,GROUP_EXIT=231};
#else
enum {READ=3,WRITE=4,CLOSE=6,POLL=168,FCNTL=55,SLEEP=162,EXIT=1,GROUP_EXIT=252};
#endif
static result call(word n, word a, word b, word c) {
    result r;
#ifdef __x86_64__
    __asm__ volatile("syscall" : "=a"(r) : "a"(n),"D"(a),"S"(b),"d"(c) : "rcx","r11","memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n),"b"(a),"c"(b),"d"(c) : "memory");
#endif
    return r;
}
static result net(unsigned op, word a, word b, word c, word d, word e) {
#ifdef __x86_64__
    word n=op==1 ? 41 : op==2 ? 49 : op==3 ? 42 : op==4 ? 50 : op==5 ? 43 : op==10 ? 45 : op==11 ? 44 : 54;
    register word r10 __asm__("r10")=d, r8 __asm__("r8")=e, r9 __asm__("r9")=0;
    result r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9) : "rcx","r11","memory");
    return r;
#else
    word args[]={a,b,c,d,e,0};
    return call(102,op,(word)args,0);
#endif
}
#define CHECK(c) do { if (!(c)) { call(GROUP_EXIT,__LINE__,0,0); __builtin_unreachable(); } } while (0)
static unsigned char stack[1024*1024] __attribute__((aligned(16)));
static word tls[8];
static unsigned descriptor[4], tid, entered;
static result shared;
extern result spawn(word top,word parent_tid,word tls,word child_tid);
__attribute__((noreturn)) void child_main(void *unused) {
    (void)unused;
    char c=0;
    __atomic_store_n(&entered,1,__ATOMIC_SEQ_CST);
    CHECK(call(READ,shared,(word)&c,1)==1 && c=='y');
    call(EXIT,0,0,0); __builtin_unreachable();
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    result fd=net(1,2,NR_CASE==2 ? 2 : 1,0,0,0);
    CHECK(fd>=0);
    unsigned char addr[16]={2,0,(NR_PORT>>8)&255,NR_PORT&255,127,0,0,1};
    if (NR_CASE!=3) CHECK(net(3,fd,(word)addr,16,0,0)==0);
    CHECK(call(FCNTL,fd,3,0)==2);
    char c=0;
    if (NR_CASE==0) {
        shared=fd; tls[0]=(word)tls;
        descriptor[0]=12; descriptor[1]=(unsigned)(word)tls; descriptor[2]=0xfffff; descriptor[3]=0x51;
        word top=(word)(stack+sizeof(stack))-16; *(word *)top=0;
#ifdef __x86_64__
        word thread_tls=(word)tls;
#else
        word thread_tls=(word)descriptor;
#endif
        CHECK(spawn(top,(word)&tid,thread_tls,(word)&tid)>0);
        result duration[2]={0,1000000};
        while (!__atomic_load_n(&entered,__ATOMIC_SEQ_CST)) CHECK(call(SLEEP,(word)duration,0,0)==0);
        duration[1]=50000000;
        CHECK(call(SLEEP,(word)duration,0,0)==0);
        for (unsigned i=0;i<32;i++) CHECK(net(10,fd,(word)&c,1,0x40,0)==-11);
        CHECK(call(FCNTL,fd,3,0)==2);
        c='R'; CHECK(call(WRITE,fd,(word)&c,1)==1);
        duration[1]=1000000;
        while (__atomic_load_n(&tid,__ATOMIC_SEQ_CST)) CHECK(call(SLEEP,(word)duration,0,0)==0);
    } else if (NR_CASE==1) {
        int size=4096;
        CHECK(net(14,fd,1,7,(word)&size,4)==0);
        static char payload[65536];
        for (unsigned i=0;i<sizeof(payload);i++) payload[i]='x';
        unsigned long long total=0;
        result n;
        do {
            n=net(11,fd,(word)payload,sizeof(payload),0x4040,0);
            CHECK(n>0 || n==-11);
            if(n>0) total+=(word)n;
            CHECK(total<64*1024*1024);
        } while(n!=-11);
        CHECK(total>0 && call(FCNTL,fd,3,0)==2);
        CHECK(call(WRITE,1,(word)&total,8)==8);
        CHECK(call(READ,fd,(word)&c,1)==1 && c=='z');
    } else if (NR_CASE==2) {
        CHECK(net(10,fd,(word)&c,1,0x40,0)==-11);
        c='u'; CHECK(net(11,fd,(word)&c,1,0x4040,0)==1);
        struct {int fd; short events,revents;} poll={(int)fd,1,0};
        CHECK(call(POLL,(word)&poll,1,5000)==1);
        CHECK(net(10,fd,(word)&c,1,0x40,0)==1 && c=='v');
        CHECK(net(10,fd,(word)&c,1,0x40,0)==-11);
        CHECK(call(FCNTL,fd,3,0)==2);
    } else {
        int enabled=1;
        CHECK(net(14,fd,1,2,(word)&enabled,4)==0);
        CHECK(net(2,fd,(word)addr,16,0,0)==0);
        CHECK(net(4,fd,1,0,0,0)==0);
        CHECK(call(FCNTL,fd,4,0x802)==0);
        CHECK(net(5,fd,0,0,0,0)==-11);
        CHECK(call(FCNTL,fd,4,2)==0);
        c='L'; CHECK(call(WRITE,1,(word)&c,1)==1);
        result accepted=net(5,fd,0,0,0,0);
        CHECK(accepted>=0 && call(FCNTL,accepted,3,0)==2);
        c='x'; CHECK(call(WRITE,accepted,(word)&c,1)==1);
        CHECK(call(READ,accepted,(word)&c,1)==1 && c=='y');
        CHECK(call(CLOSE,accepted,0,0)==0);
    }
    CHECK(call(CLOSE,fd,0,0)==0);
    call(EXIT,42,0,0); __builtin_unreachable();
}

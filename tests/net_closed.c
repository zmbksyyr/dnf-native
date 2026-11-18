/* Observe consumed TCP errors and thread-directed SIGPIPE through Linux UAPI. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {READ=0, WRITE=1, CLOSE=3, POLL=7, ACTION=13, MASK=14, SLEEP=35, GETPID=39, GETTID=186, EXIT=60, GROUP_EXIT=231};
#else
enum {READ=3, WRITE=4, CLOSE=6, POLL=168, ACTION=174, MASK=175, SLEEP=162, GETPID=20, GETTID=224, EXIT=1, GROUP_EXIT=252};
#endif
static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word extra __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(extra) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
    return r;
}
static result net(unsigned op, word a, word b, word c, word d, word e, word f) {
#ifdef __x86_64__
    word n=op==1 ? 41 : op==3 ? 42 : op==13 ? 48 : op==15 ? 55 : 44;
    register word r10 __asm__("r10")=d, r8 __asm__("r8")=e, r9 __asm__("r9")=f;
    result r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
#else
    word args[]={a,b,c,d,e,f};
    return call(102,op,(word)args,0,0);
#endif
}
#define CHECK(c) do { if (!(c)) { call(GROUP_EXIT,__LINE__,0,0,0); __builtin_unreachable(); } } while (0)
static volatile unsigned caught;
static word writer_tid;
static unsigned char worker_stack[1024*1024] __attribute__((aligned(16)));
static word worker_tls[8];
static unsigned worker_descriptor[4], worker_tid, worker_done;
extern result spawn(word stack, word parent_tid, word tls, word child_tid);
__attribute__((noreturn)) void child_main(void *unused) {
    (void)unused;
    unsigned long long mask=1ULL<<12;
    CHECK(call(MASK,1,(word)&mask,0,8)==0);
    for (unsigned i=0;i<1000;i++) call(GETPID,0,0,0,0);
    CHECK(caught==0); /* Another unblocked thread must not consume the writer's signal. */
    __atomic_store_n(&worker_done,1,__ATOMIC_SEQ_CST);
    call(EXIT,0,0,0,0); __builtin_unreachable();
}
static void check_other_thread(void) {
    word top=(word)(worker_stack+sizeof(worker_stack))-16;
    *(word *)top=0;
    worker_tls[0]=(word)worker_tls;
    worker_descriptor[0]=12; worker_descriptor[1]=(unsigned)(word)worker_tls;
    worker_descriptor[2]=0xfffff; worker_descriptor[3]=0x51;
#ifdef __x86_64__
    word tls=(word)worker_tls;
#else
    word tls=(word)worker_descriptor;
#endif
    CHECK(spawn(top,(word)&worker_tid,tls,(word)&worker_tid)>0);
    result duration[2]={0,1000000};
    while (!__atomic_load_n(&worker_done,__ATOMIC_SEQ_CST) || __atomic_load_n(&worker_tid,__ATOMIC_SEQ_CST))
        CHECK(call(SLEEP,(word)duration,0,0,0)==0);
    CHECK(caught==0);
}
static void handler(int signo, const int *info, void *context) {
    CHECK(signo==13 && info && context && info[0]==13 && info[1]==0 && info[2]==0);
    CHECK((word)info[sizeof(word)==8 ? 4 : 3]==(word)call(GETPID,0,0,0,0));
    CHECK((word)call(GETTID,0,0,0,0)==writer_tid);
    ++caught;
}
__attribute__((naked)) static void restore(void) {
#ifdef __x86_64__
    __asm__ volatile("mov $15,%eax; syscall; ud2");
#else
    __asm__ volatile("mov $173,%eax; int $0x80; ud2");
#endif
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    struct {word handler,flags,restorer; unsigned long long mask;} action={
        NR_SIGNAL==0 ? 1 : NR_SIGNAL==3 ? 0 : (word)handler,0x04000004,(word)restore,0};
    writer_tid=(word)call(GETTID,0,0,0,0);
    CHECK(call(ACTION,13,(word)&action,0,8)==0);
    unsigned long long mask=1ULL<<12;
    if (NR_SIGNAL==2) CHECK(call(MASK,0,(word)&mask,0,8)==0);
    result fd=net(1,2,NR_CASE==5 ? 2 : 1,0,0,0,0);
    CHECK(fd>=0);
    char byte='x';
    if (NR_CASE==5) {
        CHECK(call(WRITE,fd,(word)&byte,1,0)==-89);
        CHECK(caught==0);
    } else {
        if (NR_CASE!=4) {
            unsigned char address[16]={2,0,(NR_PORT>>8)&255,NR_PORT&255,127,0,0,1};
            CHECK(net(3,fd,(word)address,16,0,0,0)==0);
            CHECK(call(WRITE,fd,(word)&byte,1,0)==1);
            if (NR_CASE<3 || NR_CASE==6) {
                struct {int fd; short events,revents;} poll={(int)fd,1,0};
                CHECK(call(POLL,(word)&poll,1,5000,0)==1);
                if (NR_CASE==6) {
                    result duration[2]={0,100000000};
                    CHECK(call(SLEEP,(word)duration,0,0,0)==0);
                }
                if (NR_CASE==0) CHECK(call(READ,fd,(word)&byte,1,0)==-104);
                if (NR_CASE==1) CHECK(call(WRITE,fd,(word)&byte,1,0)==-104);
                if (NR_CASE==2 || NR_CASE==6) {
                    int error=0,length=4;
                    CHECK(net(15,fd,1,4,(word)&error,(word)&length,0)==0 && error==104);
                }
                if (NR_CASE==6) CHECK(call(READ,fd,(word)&byte,1,0)==1 && byte=='y');
                CHECK(call(READ,fd,(word)&byte,1,0)==0);
                int error=-1,length=4;
                CHECK(net(15,fd,1,4,(word)&error,(word)&length,0)==0 && error==0);
            } else CHECK(net(13,fd,1,0,0,0,0)==0);
        }
        CHECK(net(11,fd,(word)&byte,1,0x4000,0,0)==-32);
        CHECK(caught==0); /* MSG_NOSIGNAL suppresses only this send. */
        CHECK(call(WRITE,fd,(word)&byte,1,0)==-32);
        CHECK(caught==(NR_SIGNAL==1 ? 1u : 0u));
        CHECK(call(WRITE,fd,(word)&byte,0,0)==-32);
        CHECK(caught==(NR_SIGNAL==1 ? 2u : 0u));
        if (NR_SIGNAL==2) {
            check_other_thread();
            CHECK(call(MASK,1,(word)&mask,0,8)==0);
            CHECK(caught==1); /* Standard blocked signals coalesce. */
        }
        if (NR_CASE==3) CHECK(call(READ,fd,(word)&byte,1,0)==1 && byte=='y');
    }
    CHECK(call(CLOSE,fd,0,0,0)==0);
    const char ok[]="closed-socket-ok\n";
    CHECK(call(WRITE,1,(word)ok,sizeof(ok)-1,0)==sizeof(ok)-1);
    call(EXIT,42,0,0,0); __builtin_unreachable();
}

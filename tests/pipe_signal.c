/* Linux broken-pipe write/writev: return value and actual signal disposition. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {WRITE=1, WRITEV=20, PIPE=22, CLOSE=3, ACTION=13, MASK=14, GETTID=186, EXIT=60};
#else
enum {WRITE=4, WRITEV=146, PIPE=42, CLOSE=6, ACTION=174, MASK=175, GETTID=224, EXIT=1};
#endif
static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word fourth __asm__("r10")=d;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(fourth):"rcx","r11","memory");
#else
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d):"memory");
#endif
    return r;
}
#define CHECK(c) do { if (!(c)) { call(EXIT,200+__LINE__%50,0,0,0); __builtin_unreachable(); } } while (0)
#ifndef NR_DEFAULT
static volatile unsigned seen;
static word writer_tid;
__attribute__((naked)) static void restore(void) {
#ifdef __x86_64__
    __asm__ volatile("mov $15, %eax; syscall; ud2");
#else
    __asm__ volatile("mov $173, %eax; int $0x80; ud2");
#endif
}
static void handler(int signal, void *info, void *context) {
    CHECK(signal==13 && info && context);
    const int *fields=(const int *)info;
    CHECK(fields[0]==13 && fields[1]==0 && fields[2]==0);
    CHECK((word)call(GETTID,0,0,0,0)==writer_tid);
    unsigned long long mask=0;
    CHECK(call(MASK,0,0,(word)&mask,8)==0 && (mask & (1ULL<<12)));
    ++seen;
}
#endif
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    int fds[2];
    CHECK(call(PIPE,(word)fds,0,0,0)==0);
    CHECK(call(CLOSE,fds[0],0,0,0)==0);
    const char payload[]="pipe";
#ifdef NR_DEFAULT
    CHECK(call(WRITE,fds[1],(word)payload,sizeof(payload),0)==-32);
    /* A default SIGPIPE must terminate before write returns here. */
    call(EXIT,99,0,0,0);
#else
    struct { word base,length; } vector={(word)payload,sizeof(payload)};
    writer_tid=(word)call(GETTID,0,0,0,0);
    struct { word handler,flags,restorer; unsigned long long mask; } action={
        (word)handler,0x04000004,(word)restore,0
    };
    CHECK(call(ACTION,13,(word)&action,0,8)==0);
    CHECK(call(WRITE,fds[1],(word)payload,0,0)==0 && seen==0);
    CHECK(call(WRITE,fds[1],(word)payload,sizeof(payload),0)==-32 && seen==1);
    CHECK(call(WRITEV,fds[1],(word)&vector,1,0)==-32 && seen==2);
    unsigned long long mask=1ULL<<12;
    CHECK(call(MASK,0,(word)&mask,0,8)==0);
    CHECK(call(WRITE,fds[1],(word)payload,sizeof(payload),0)==-32 && seen==2);
    CHECK(call(WRITEV,fds[1],(word)&vector,1,0)==-32 && seen==2);
    /* Ordinary pending signals coalesce while blocked. */
    CHECK(call(MASK,1,(word)&mask,0,8)==0 && seen==3);
    action.handler=1;
    CHECK(call(ACTION,13,(word)&action,0,8)==0);
    CHECK(call(WRITE,fds[1],(word)payload,sizeof(payload),0)==-32 && seen==3);
    CHECK(call(WRITEV,fds[1],(word)&vector,1,0)==-32 && seen==3);
    CHECK(call(CLOSE,fds[1],0,0,0)==0);
    CHECK(call(WRITE,fds[1],(word)payload,sizeof(payload),0)==-9 && seen==3);
    const char message[]="native-pipe-signal-ok\n";
    CHECK(call(WRITE,1,(word)message,sizeof(message)-1,0)==sizeof(message)-1);
    call(EXIT,42,0,0,0);
#endif
    __builtin_unreachable();
}

typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {READ=0,WRITE=1,CLOSE=3,SELECT=23,POLL=7,EVENTFD=284,EVENTFD2=290,EXIT=60,GROUP_EXIT=231,EP_CREATE=291,EP_CTL=233,EP_WAIT=232};
#else
enum {READ=3,WRITE=4,CLOSE=6,SELECT=142,POLL=168,EVENTFD=323,EVENTFD2=328,EXIT=1,GROUP_EXIT=252,EP_CREATE=329,EP_CTL=255,EP_WAIT=256};
#endif
static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word fourth __asm__("r10") = d;
    register word fifth __asm__("r8") = 0;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n),"D"(a),"S"(b),"d"(c),"r"(fourth),"r"(fifth) : "rcx","r11","memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"((word)0) : "memory");
#endif
    return r;
}
#define CHECK(x) do {if (!(x)) {call(GROUP_EXIT,__LINE__,0,0,0); __builtin_unreachable();}} while (0)
static result io(word n, result fd, unsigned long long *value) { return call(n,(word)fd,(word)value,8,0); }
struct Poll {int fd;short events,revents;};
static unsigned char child_stack[1024 * 1024] __attribute__((aligned(16)));
static word tls[8];
static unsigned tid, descriptor[4];
extern result spawn(word stack, word parent_tid, word tls, word child_tid);
static result blocking;
__attribute__((noreturn)) void child_main(void *unused) {
    (void)unused;
    unsigned long long value=0;
    CHECK(io(READ,blocking,&value)==8 && value==37);
    value=1; CHECK(io(WRITE,blocking,&value)==8);
    call(EXIT,0,0,0,0); __builtin_unreachable();
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    CHECK(call(EVENTFD2,0,2,0,0)==-22);
    result fd=call(EVENTFD2,0,0x80800,0,0); CHECK(fd>=0);
    unsigned long long value=0;
    CHECK(io(READ,fd,&value)==-11);
    CHECK(call(READ,fd,(word)&value,7,0)==-22);
    CHECK(call(WRITE,fd,(word)&value,9,0)==-22);
    CHECK(call(READ,fd,1,8,0)==-14);
    value=~0ULL; CHECK(io(WRITE,fd,&value)==-22);
    value=~0ULL-1; CHECK(io(WRITE,fd,&value)==8);
    value=1; CHECK(io(WRITE,fd,&value)==-11);
    struct Poll p={(int)fd,5,0}; CHECK(call(POLL,(word)&p,1,0,0)==1 && p.revents==1);
    CHECK(io(READ,fd,&value)==8 && value==~0ULL-1);
    CHECK(call(POLL,(word)&p,1,0,0)==1 && p.revents==4);
    word read_bits=(word)1<<fd, write_bits=read_bits;
    CHECK(call(SELECT,fd+1,(word)&read_bits,(word)&write_bits,0)==1 && !read_bits && write_bits==((word)1<<fd));
    result ep=call(EP_CREATE,0,0,0,0); CHECK(ep>=0);
    unsigned char event[12]={1,0,0,0,73};
    CHECK(call(EP_CTL,ep,1,fd,(word)event)==0);
    CHECK(call(EP_WAIT,ep,(word)event,1,0)==0);
    value=5; CHECK(io(WRITE,fd,&value)==8);
    CHECK(call(EP_WAIT,ep,(word)event,1,0)==1 && event[4]==73);
    CHECK(io(READ,fd,&value)==8 && value==5);
    CHECK(call(CLOSE,fd,0,0,0)==0);
    CHECK(call(EP_WAIT,ep,(word)event,1,0)==0);
    CHECK(call(CLOSE,ep,0,0,0)==0);
    fd=call(EVENTFD2,2,0x801,0,0);CHECK(fd>=0);
    CHECK(io(READ,fd,&value)==8 && value==1);
    CHECK(io(READ,fd,&value)==8 && value==1);
    CHECK(io(READ,fd,&value)==-11); CHECK(call(CLOSE,fd,0,0,0)==0);
    blocking=call(EVENTFD,0,0,0,0); CHECK(blocking>=0);
    tls[0]=(word)tls;
    descriptor[0]=12;descriptor[1]=(unsigned)(word)tls;descriptor[2]=0xfffff;descriptor[3]=0x51;
    word top=(word)(child_stack+sizeof(child_stack))-16; *(word *)top=0;
    CHECK(spawn(top,(word)&tid,
#ifdef __x86_64__
                (word)tls,
#else
                (word)descriptor,
#endif
                (word)&tid)>0);
    value=37;CHECK(io(WRITE,blocking,&value)==8);
    /* The child acknowledges using the same counter. Wait for child exit so
       the parent cannot consume its own message. */
    while (__atomic_load_n(&tid,__ATOMIC_SEQ_CST)) __asm__ volatile("pause");
    CHECK(io(READ,blocking,&value)==8 && value==1);
    CHECK(call(CLOSE,blocking,0,0,0)==0);
    const char message[]="native-eventfd-ok\n";
    CHECK(call(WRITE,1,(word)message,sizeof(message)-1,0)==sizeof(message)-1);
    call(GROUP_EXIT,42,0,0,0);__builtin_unreachable();
}

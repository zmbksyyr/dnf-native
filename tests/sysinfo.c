/* Original glibc's i386 GS:16 gateway: register/flags/FP preservation and
   signal/tracing fallbacks. Build with the pinned, unmodified glibc loader. */
typedef __UINTPTR_TYPE__ word;
extern int __libc_start_main(int (*)(void), int, char **, void *, void *, void *, void *);
extern int pthread_create(word *, const void *, void *(*)(void *), void *);
extern int pthread_join(word, void **);
extern int memcmp(const void *, const void *, unsigned);
extern int pipe(int *);
extern void _exit(int) __attribute__((noreturn));
static void fail(unsigned line) __attribute__((noreturn));
#define CHECK(c) do { if (!(c)) fail(__LINE__); } while (0)

static unsigned char original_fp[512] __attribute__((aligned(16),used));
static unsigned char before_fp[512] __attribute__((aligned(16),used));
static unsigned char after_fp[512] __attribute__((aligned(16),used));
static const unsigned vector[4] __attribute__((aligned(16),used)) = {0x12345678,0x9abcdef0,0x13579bdf,0x2468ace0};
static const unsigned mxcsr __attribute__((used)) = 0x5f80;
static unsigned before_flags __attribute__((used)),after_flags __attribute__((used));
static unsigned before_sp __attribute__((used)),before_bp __attribute__((used)),bad __attribute__((used));
static volatile unsigned seen, pipe_seen;

static int raw(unsigned number, unsigned a, unsigned b, unsigned d, unsigned s) {
    int result;
    __asm__ volatile("int $0x80":"=a"(result):"a"(number),"b"(a),"c"(b),"d"(d),"S"(s):"memory");
    return result;
}
static void fail(unsigned line) {
    char msg[]="check failed line 0000\n";
    for(unsigned i=0;i<4;++i) {msg[21-i]='0'+line%10;line/=10;}
    raw(4,2,(word)msg,sizeof(msg)-1,0);
    _exit(100);
}
static unsigned checked_call(unsigned number, unsigned argument, unsigned third, unsigned fourth) {
    unsigned result, a=argument, c=third, d=fourth;
    bad=0;
    __asm__ volatile(
        "fxsave original_fp\n\tfninit\n\tfldpi\n\tfld1\n\tldmxcsr mxcsr\n\t"
        "movdqa vector, %%xmm0\n\tmovdqa vector, %%xmm1\n\tmovdqa vector, %%xmm2\n\t"
        "movdqa vector, %%xmm3\n\tmovdqa vector, %%xmm4\n\tmovdqa vector, %%xmm5\n\t"
        "movdqa vector, %%xmm6\n\tmovdqa vector, %%xmm7\n\tfxsave before_fp\n\t"
        "movl $0x3456789a, %%esi\n\tmovl $0x456789ab, %%edi\n\t"
        "movl %%esp, before_sp\n\tmovl %%ebp, before_bp\n\t"
        "pushl $0xcd7\n\tpopfl\n\tpushfl\n\tpopl before_flags\n\t"
        "call *%%gs:16\n\tpushfl\n\tpopl after_flags\n\tcld\n\tfxsave after_fp\n\t"
        "cmpl $0x3456789a, %%esi\n\tjne 1f\n\tcmpl $0x456789ab, %%edi\n\tjne 1f\n\t"
        "cmpl %%esp, before_sp\n\tjne 1f\n\tcmpl %%ebp, before_bp\n\tjne 1f\n\tjmp 2f\n\t"
        "1: movl $1, bad\n\t2: fxrstor original_fp"
        : "=a"(result), "+b"(a), "+c"(c), "+d"(d) : "a"(number)
        : "esi","edi","memory","cc");
    CHECK(!bad && !((before_flags ^ after_flags) & 0xcd5));
    CHECK(memcmp(before_fp,after_fp,sizeof(before_fp))==0);
    CHECK(a==argument && c==third && d==fourth);
    return result;
}
static unsigned query(unsigned number, unsigned argument) {
    return checked_call(number,argument,0x12345678,0x23456789);
}
static int socket_call(unsigned op, word a, word b, word c, word d, word e, word f) {
    word args[]={a,b,c,d,e,f};
    return raw(102,op,(word)args,0,0);
}
__attribute__((naked)) static void restore(void);
static void pipe_handler(int signal, void *info, void *context) {
    (void)info; (void)context; CHECK(signal==13); ++pipe_seen;
}
static void socket_state_test(void) {
    const char payload[]="socket-gateway-bytes";
    char buffer[sizeof(payload)];
    unsigned char address[16]={2,0,0,0,127,0,0,1};
    unsigned length=16;
    int receiver=socket_call(1,2,2,0,0,0,0),sender=socket_call(1,2,2,0,0,0,0);
    CHECK(receiver>=0 && sender>=0);
    CHECK(socket_call(2,receiver,(word)address,16,0,0,0)==0);
    CHECK(socket_call(6,receiver,(word)address,(word)&length,0,0,0)==0);
    word send_args[]={sender,(word)payload,sizeof(payload),0x4040,(word)address,16};
    word recv_args[]={receiver,(word)buffer,sizeof(buffer),0x40,0,0};
    CHECK((int)checked_call(102,12,(word)recv_args,0)==-11);
    CHECK(checked_call(102,11,(word)send_args,0)==sizeof(payload));
    CHECK(checked_call(102,12,(word)recv_args,0)==sizeof(buffer));
    CHECK(memcmp(buffer,payload,sizeof(buffer))==0);
    CHECK(raw(55,receiver,4,0x802,0)==0);
    CHECK((int)checked_call(3,receiver,(word)buffer,sizeof(buffer))==-11);
    CHECK(checked_call(102,11,(word)send_args,0)==sizeof(payload));
    CHECK(checked_call(3,receiver,(word)buffer,sizeof(buffer))==sizeof(buffer));
    CHECK(memcmp(buffer,payload,sizeof(buffer))==0);
    CHECK(checked_call(102,11,(word)send_args,0)==sizeof(payload));
    CHECK((int)checked_call(3,receiver,1,1)==-14);
    CHECK((int)checked_call(102,10,1,0)==-14);
    CHECK(checked_call(102,11,(word)send_args,0)==sizeof(payload));
    recv_args[1]=1;
    CHECK((int)checked_call(102,10,(word)recv_args,0)==-14);
    CHECK(raw(6,receiver,0,0,0)==0 && raw(6,sender,0,0,0)==0);
    CHECK((int)checked_call(3,receiver,(word)buffer,sizeof(buffer))==-9);

    int listener=socket_call(1,2,1,0,0,0,0);
    address[2]=address[3]=0;
    CHECK(listener>=0 && socket_call(2,listener,(word)address,16,0,0,0)==0);
    CHECK(socket_call(6,listener,(word)address,(word)&length,0,0,0)==0);
    CHECK(socket_call(4,listener,1,0,0,0,0)==0);
    sender=socket_call(1,2,1,0,0,0,0);
    CHECK(sender>=0 && socket_call(3,sender,(word)address,16,0,0,0)==0);
    receiver=socket_call(5,listener,0,0,0,0,0);
    CHECK(receiver>=0 && raw(6,listener,0,0,0)==0);
    CHECK(raw(55,receiver,4,0x802,0)==0);
    CHECK(raw(55,sender,4,0x802,0)==0);
    word stream_send[]={sender,(word)payload,sizeof(payload),0x4000};
    word stream_recv[]={receiver,(word)buffer,sizeof(buffer),0};
    CHECK((int)checked_call(102,10,(word)stream_recv,0)==-11);
    CHECK(checked_call(102,9,(word)stream_send,0)==sizeof(payload));
    struct {int fd;short events,revents;} poll={receiver,1,0};
    CHECK(raw(168,(word)&poll,1,2000,0)==1);
    CHECK(checked_call(102,10,(word)stream_recv,0)==sizeof(payload));
    CHECK(memcmp(buffer,payload,sizeof(buffer))==0);
    CHECK(checked_call(3,receiver,1,0)==0);
    int linger[2]={1,0};
    CHECK(socket_call(14,sender,1,13,(word)linger,sizeof(linger),0)==0);
    CHECK(raw(6,sender,0,0,0)==0);
    CHECK(raw(168,(word)&poll,1,2000,0)==1);
    CHECK((int)checked_call(3,receiver,(word)buffer,1)==-104);
    CHECK(checked_call(102,10,(word)stream_recv,0)==0);
    stream_send[0]=receiver;
    struct { word handler,flags,restorer; unsigned long long mask; } action={(word)pipe_handler,0x04000004,(word)restore,0};
    CHECK(raw(174,13,(word)&action,0,8)==0);
    CHECK((int)checked_call(102,9,(word)stream_send,0)==-32 && pipe_seen==0);
    stream_send[3]=0x40; /* No MSG_NOSIGNAL: retain the signal-capable dispatcher. */
    CHECK((int)checked_call(102,9,(word)stream_send,0)==-32 && pipe_seen==1);
    CHECK(raw(6,receiver,0,0,0)==0);
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
    const char filename[]="/tmp/sysinfo-write.bin", payload[]="direct-write-payload\n";
    int fd=raw(5,(word)filename,0x242,0600,0);
    CHECK(fd>=0);
    CHECK(checked_call(4,fd,(word)payload,sizeof(payload))==sizeof(payload));
    CHECK(raw(19,fd,0,0,0)==0);
    char bytes[sizeof(payload)];
    CHECK(raw(3,fd,(word)bytes,sizeof(bytes),0)==sizeof(bytes));
    CHECK(memcmp(bytes,payload,sizeof(bytes))==0);
    CHECK((int)checked_call(4,fd,1,1)==-14);
    CHECK(checked_call(4,fd,1,0)==0);
    CHECK(raw(6,fd,0,0,0)==0);
    CHECK((int)checked_call(4,fd,(word)payload,sizeof(payload))==-9);
    fd=raw(5,(word)filename,0,0,0);
    CHECK(fd>=0 && (int)checked_call(4,fd,(word)payload,sizeof(payload))==-9);
    CHECK(raw(6,fd,0,0,0)==0);
    CHECK(raw(10,(word)filename,0,0,0)==0);
    int fds[2];
    CHECK(pipe(fds)==0);
    CHECK(checked_call(4,fds[1],(word)payload,sizeof(payload))==sizeof(payload));
    CHECK(raw(3,fds[0],(word)bytes,sizeof(bytes),0)==sizeof(bytes));
    CHECK(memcmp(bytes,payload,sizeof(bytes))==0);
    CHECK(raw(6,fds[0],0,0,0)==0);
    /* Ignore SIGPIPE, but still require the real broken-pipe return. */
    struct { word handler,flags,restorer; unsigned long long mask; } ignore={1,0,0,0};
    CHECK(raw(174,13,(word)&ignore,0,8)==0);
    CHECK((int)checked_call(4,fds[1],(word)payload,sizeof(payload))==-32);
    CHECK(raw(6,fds[1],0,0,0)==0);
    socket_state_test();
    /* A self-directed blocked signal stays pending across the fast candidates,
       then runs when unblocked; delivery must never disappear in a shortcut. */
    struct { word handler,flags,restorer; unsigned long long mask; } action={(word)handler,0x04000004,(word)restore,0};
    unsigned long long mask=1ULL<<11;
    CHECK(raw(174,12,(word)&action,0,8)==0);
    CHECK(raw(175,0,(word)&mask,0,8)==0);
    CHECK(raw(37,query(20,0),12,0,0)==0);
    CHECK(query(13,0)>0 && seen==0);
    CHECK(checked_call(4,1,1,0)==0 && seen==0);
    CHECK(raw(175,1,(word)&mask,0,8)==0 && seen==1);
    int signal_socket=socket_call(1,2,2,0,0,0,0);
    CHECK(signal_socket>=0);
    unsigned char signal_address[16]={2,0,0,0,127,0,0,1};
    CHECK(socket_call(2,signal_socket,(word)signal_address,16,0,0,0)==0);
    char signal_byte;
    word signal_receive[]={signal_socket,(word)&signal_byte,1,0x40};
    const char ready[]="sysinfo-signal-ready\n";
    CHECK(raw(4,1,(word)ready,sizeof(ready)-1,0)==sizeof(ready)-1);
    for (unsigned i=0;seen==1 && i<10000000;++i)
        CHECK((int)checked_call(102,10,(word)signal_receive,0)==-11);
    CHECK(seen==2);
    CHECK(raw(6,signal_socket,0,0,0)==0);
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

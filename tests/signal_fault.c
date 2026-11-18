/* Actual memory faults, Linux siginfo/ucontext and rt_sigreturn recovery. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
static result call(word n,word a,word b,word c,word d,word e,word f) {
    result r;
#ifdef __x86_64__
    register word fourth __asm__("r10")=d, fifth __asm__("r8")=e, sixth __asm__("r9")=f;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(fourth),"r"(fifth),"r"(sixth):"rcx","r11","memory");
#else
    register word sixth __asm__("ebp")=f;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e),"r"(sixth):"memory");
#endif
    return r;
}
#ifdef __x86_64__
enum {WRITE=1,MMAP=9,PROTECT=10,ADVISE=28,ACTION=13,MASK=14,EXIT=60};
#define PTR_REG "%%rax"
typedef struct {word r8,r9,r10,r11,r12,r13,r14,r15,rdi,rsi,rbp,rbx,rdx,rax,rcx,rsp,pc,flags;unsigned short cs,gs,fs,ss;word err,trap,oldmask,address,fpstate,reserved[8];} Machine;
#else
enum {WRITE=4,MMAP=192,PROTECT=125,ADVISE=219,ACTION=174,MASK=175,EXIT=1};
#define PTR_REG "%%eax"
typedef struct {word gs,fs,es,ds,edi,esi,ebp,esp,ebx,edx,ecx,rax,trap,err,pc,cs,flags,esp_signal,ss,fpstate,oldmask,address;} Machine;
#endif
typedef struct {word flags,link,stack;unsigned stack_flags;
#ifdef __x86_64__
unsigned pad;
#endif
word stack_size;Machine machine;unsigned char mask[8];} UserContext;
static volatile unsigned seen;
static word wanted_address;
static unsigned wanted_code,wanted_write;
#define CHECK(x) do {if(!(x)){call(EXIT,__LINE__,0,0,0,0,0);__builtin_unreachable();}} while(0)
static void handler(int signal, unsigned char *info, UserContext *u) {
    CHECK(signal==11 && *(int *)info==11 && *(int *)(info+8)==(int)wanted_code);
    CHECK(*(word *)(info+(sizeof(word)==8?16:12))==wanted_address);
    CHECK(u->machine.address==wanted_address && u->machine.trap==14);
    CHECK((u->machine.err & 2)==wanted_write && (u->machine.err & 4)==4);
    CHECK((u->machine.err & 1)==(wanted_code==2));
    ++seen;
    u->machine.pc+=2; /* Our two-byte MOV, checked by the resumed result below. */
    u->machine.rax=123;
}
static void fault(word address,unsigned code,unsigned write) {
    wanted_address=address;wanted_code=code;wanted_write=write;
    word value;
    if(write) __asm__ volatile("movb %%dl,(" PTR_REG ")":"=a"(value):"a"(address),"d"(85):"memory");
    else __asm__ volatile("movb (" PTR_REG "),%%dl":"=a"(value):"a"(address):"edx","memory");
    CHECK(value==123);
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    struct {word handler,flags,restorer;unsigned long long mask;} action={(word)handler,4,0,0};
    CHECK(call(ACTION,11,(word)&action,0,8,0,0)==0);
#ifdef NR_BLOCKED
    unsigned long long mask=1ULL<<10;
    CHECK(call(MASK,0,(word)&mask,0,8,0,0)==0);
#endif
    fault(1,1,0);
    word page=(word)call(MMAP,0,4096,3,0x22,(word)-1,0);
    CHECK(page && page<(word)-4095);
    CHECK(call(PROTECT,page,4096,1,0,0,0)==0);
    CHECK(call(ADVISE,page,4096,4,0,0,0)==0);
    fault(page,2,2);
    CHECK(call(PROTECT,page,4096,0,0,0,0)==0);
    CHECK(call(ADVISE,page,4096,4,0,0,0)==0);
    fault(page,2,0);
    CHECK(seen==3);
    CHECK(call(WRITE,1,(word)"native-fault-recovery-ok\n",sizeof("native-fault-recovery-ok\n")-1,0,0,0)==sizeof("native-fault-recovery-ok\n")-1);
    call(EXIT,42,0,0,0,0,0);__builtin_unreachable();
}

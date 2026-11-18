/* Concurrent parents must receive independent clone contexts and TLS. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;
#ifdef __x86_64__
enum {SLEEP=35,EXIT=60,GROUP_EXIT=231,GETTID=186};
#else
enum {SLEEP=162,EXIT=1,GROUP_EXIT=252,GETTID=224};
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
#define CHECK(c) do { if (!(c)) { call(GROUP_EXIT,__LINE__,0,0); __builtin_unreachable(); } } while (0)
#define PARENTS 24
static unsigned char stacks[PARENTS*2][1024*1024] __attribute__((aligned(16)));
static word tls[PARENTS*2][8];
static unsigned descriptors[PARENTS*2][4], tids[PARENTS*2], ready, go, finished;
extern result spawn(word top,word parent_tid,word tls,word child_tid);
static void pause_thread(void) { result duration[2]={0,1000000}; CHECK(call(SLEEP,(word)duration,0,0)==0); }
static void create(unsigned i) {
    tls[i][0]=(word)tls[i]; tls[i][1]=i;
    descriptors[i][0]=12; descriptors[i][1]=(unsigned)(word)tls[i];
    descriptors[i][2]=0xfffff; descriptors[i][3]=0x51;
    word top=(word)(stacks[i]+sizeof(stacks[i]))-16;
    *(word *)top=i;
#ifdef __x86_64__
    word thread_tls=(word)tls[i];
#else
    word thread_tls=(word)descriptors[i];
#endif
    CHECK(spawn(top,(word)&tids[i],thread_tls,(word)&tids[i])>0);
}
__attribute__((noreturn)) void child_main(void *arg) {
    unsigned i=(unsigned)(word)arg;
    word current;
#ifdef __x86_64__
    word base=0;
    CHECK(call(158,0x1003,(word)&base,0)==0);
    CHECK(base==(word)tls[i]);
    current=((word *)base)[1];
#else
    __asm__ volatile("mov $0x63,%%ax; mov %%ax,%%gs":::"eax");
    __asm__ volatile("mov %%gs:4,%0":"=r"(current));
#endif
    CHECK(current==i);
    CHECK((unsigned)call(GETTID,0,0,0)==__atomic_load_n(&tids[i],__ATOMIC_SEQ_CST));
    if(i<PARENTS) {
        __atomic_add_fetch(&ready,1,__ATOMIC_SEQ_CST);
        while(!__atomic_load_n(&go,__ATOMIC_SEQ_CST)) pause_thread();
        create(i+PARENTS);
    } else __atomic_add_fetch(&finished,1,__ATOMIC_SEQ_CST);
    call(EXIT,0,0,0); __builtin_unreachable();
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    for(unsigned i=0;i<PARENTS;i++) create(i);
    while(__atomic_load_n(&ready,__ATOMIC_SEQ_CST)!=PARENTS) pause_thread();
    __atomic_store_n(&go,1,__ATOMIC_SEQ_CST);
    while(__atomic_load_n(&finished,__ATOMIC_SEQ_CST)!=PARENTS) pause_thread();
    for(unsigned i=0;i<PARENTS*2;i++) while(__atomic_load_n(&tids[i],__ATOMIC_SEQ_CST)) pause_thread();
    call(GROUP_EXIT,42,0,0); __builtin_unreachable();
}

/* Exercise queue movement and conditional atomic wakes with real guest threads. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

#ifdef __x86_64__
enum {FUTEX=202, EXIT=60, GROUP_EXIT=231, WRITE=1, SLEEP=35, MMAP=9, MPROTECT=10, MUNMAP=11, OPEN=2, READ=0, CLOSE=3};
#else
enum {FUTEX=240, EXIT=1, GROUP_EXIT=252, WRITE=4, SLEEP=162, MMAP=192, MPROTECT=125, MUNMAP=91, OPEN=5, READ=3, CLOSE=6};
#endif

static result call(word n, word a, word b, word c, word d, word e, word f) {
    result value;
#ifdef __x86_64__
    register word r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(value) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
#else
    register word bp __asm__("ebp") = f;
    __asm__ volatile("int $0x80" : "=a"(value) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "r"(bp) : "memory");
#endif
    return value;
}

#define CHECK(c) do { if (!(c)) { call(GROUP_EXIT, __LINE__, 0, 0, 0, 0, 0); __builtin_unreachable(); } } while (0)


static unsigned arrived, phase;
struct Worker {
    unsigned char stack[1024 * 1024] __attribute__((aligned(16)));
    word tls[8];
    unsigned descriptor[4], tid, done;
};
static struct Worker workers[8];
extern result spawn(word stack, word parent_tid, word tls, word child_tid);
static void barrier(unsigned next) {
    __atomic_add_fetch(&arrived,1,__ATOMIC_SEQ_CST);
    while(__atomic_load_n(&phase,__ATOMIC_SEQ_CST)<next) __asm__ volatile("pause");
}
__attribute__((noreturn)) void child_main(struct Worker *worker) {
    unsigned index=(unsigned)(worker-workers);
    for(unsigned round=0;round<200;++round) {
        barrier(round+1);
        result p=call(MMAP,0,4096,3,0x22,(word)-1,0);
        CHECK((word)p < (word)-4095 && p != 0);
        volatile word *memory=(volatile word *)(word)p;
        word sentinel=(word)worker ^ (round+1);
        memory[0]=sentinel;memory[511/sizeof(word)]=sentinel;
        CHECK(call(MPROTECT,(word)p,4096,3,0,0,0)==0);
        CHECK(memory[0]==sentinel && memory[511/sizeof(word)]==sentinel);
        CHECK(call(MUNMAP,(word)p,4096,0,0,0,0)==0);
        __atomic_store_n(&worker->done,round+1,__ATOMIC_SEQ_CST);
    }
    (void)index;
    call(EXIT,0,0,0,0,0,0);__builtin_unreachable();
}
static void start(unsigned index) {
    struct Worker *w = &workers[index];
    word top = (word)(w->stack + sizeof(w->stack)) - 16;
    *(word *)top = (word)w;
    w->tls[0] = (word)w->tls;
    w->descriptor[0] = 12; w->descriptor[1] = (unsigned)(word)w->tls;
    w->descriptor[2] = 0xfffff; w->descriptor[3] = 0x51;
#ifdef __x86_64__
    word tls = (word)w->tls;
#else
    word tls = (word)w->descriptor;
#endif
    CHECK(spawn(top, (word)&w->tid, tls, (word)&w->tid) > 0);
}


__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    for(unsigned i=0;i<8;++i)start(i);
    for(unsigned round=0;round<200;++round) {
        while(__atomic_load_n(&arrived,__ATOMIC_SEQ_CST)<8*(round+1)) __asm__ volatile("pause");
        __atomic_store_n(&phase,round+1,__ATOMIC_SEQ_CST);
        result fd=call(OPEN,(word)"/proc/self/maps",0,0,0,0,0);
        CHECK(fd>=0);
        char snapshot[1024];
        result count;
        do { count=call(READ,(word)fd,(word)snapshot,sizeof(snapshot),0,0,0); CHECK(count>=0); } while(count);
        CHECK(call(CLOSE,(word)fd,0,0,0,0,0)==0);
        for(unsigned i=0;i<8;++i)while(__atomic_load_n(&workers[i].done,__ATOMIC_SEQ_CST)<round+1) __asm__ volatile("pause");
    }
    const char message[]="native-concurrent-mappings-ok\n";
    CHECK(call(WRITE,1,(word)message,sizeof(message)-1,0,0,0)==sizeof(message)-1);
    call(GROUP_EXIT,42,0,0,0,0,0);__builtin_unreachable();
}

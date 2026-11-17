/* Controlled CPU and resident memory for the controller's process-tree test. */
typedef __UINTPTR_TYPE__ word;
typedef __INTPTR_TYPE__ result;

static result call(word n, word a, word b, word c, word d) {
    result r;
#ifdef __x86_64__
    register word fourth __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(fourth) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
    return r;
}
#ifdef __x86_64__
enum {WRITE=1, ACTION=13, GETPID=39, EXIT=60};
#else
enum {WRITE=4, ACTION=174, GETPID=20, EXIT=1};
#endif

static volatile unsigned char memory[48*1024*1024];
static volatile unsigned stopped;
static void stop(int signal) { (void)signal; stopped=1; }

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct {word handler, flags, restorer; unsigned long long mask;} action={(word)stop,0,0,0};
    if(call(ACTION,15,(word)&action,0,8)) {
        call(EXIT,1,0,0,0); __builtin_unreachable();
    }
    for(unsigned i=0;i<sizeof(memory);i+=4096) memory[i]=1;
    call(WRITE,1,(word)"metrics-ready\n",14,0);
    volatile unsigned value=1;
    while(!stopped) {
        for(unsigned i=0;i<5000000;++i) value=value*1664525u+1013904223u;
        memory[0]=(unsigned char)value;
        call(GETPID,0,0,0,0);
    }
    call(EXIT,0,0,0,0); __builtin_unreachable();
}

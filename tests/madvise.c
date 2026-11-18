/* Linux-visible DONTNEED contents and permissions, independent of Windows VM. */
typedef __SIZE_TYPE__ size_t;
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
enum {WRITE=1,MMAP=9,PROTECT=10,UNMAP=11,ADVISE=28,EXIT=60};
#else
enum {WRITE=4,MMAP=192,PROTECT=125,UNMAP=91,ADVISE=219,EXIT=1};
#endif
#define CHECK(c) do {if(!(c)){call(EXIT,__LINE__,0,0,0,0,0);__builtin_unreachable();}} while(0)
#define PROT(a,n,p) call(PROTECT,(a),(n),(p),0,0,0)
#define DROP(a,n) call(ADVISE,(a),(n),4,0,0,0)

#ifdef NR_CONCURRENT
static unsigned char stack[1024*1024] __attribute__((aligned(16)));
static word tls[8], target;
static unsigned descriptor[4], tid, ready, done, reads;
extern result spawn(word,word,word,word);
__attribute__((noreturn,force_align_arg_pointer)) void child_main(void *unused) {
    (void)unused;
    __atomic_store_n(&ready,1,__ATOMIC_SEQ_CST);
    while(!__atomic_load_n(&done,__ATOMIC_SEQ_CST)) {
        CHECK(*(volatile unsigned char *)target==0);
        __atomic_add_fetch(&reads,1,__ATOMIC_SEQ_CST);
    }
    call(EXIT,0,0,0,0,0,0);__builtin_unreachable();
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    target=(word)call(MMAP,0,4096,1,0x22,(word)-1,0);
    CHECK(target && target<(word)-4095);
    tls[0]=(word)tls;
    descriptor[0]=12;descriptor[1]=(unsigned)(word)tls;descriptor[2]=0xfffff;descriptor[3]=0x51;
    word top=(word)(stack+sizeof(stack))-16;
    *(word *)top=0;
#ifdef __x86_64__
    word thread_tls=(word)tls;
#else
    word thread_tls=(word)descriptor;
#endif
    CHECK(spawn(top,(word)&tid,thread_tls,(word)&tid)>0);
    while(!__atomic_load_n(&ready,__ATOMIC_SEQ_CST))__asm__ volatile("pause");
    for(unsigned i=0;i<2000;++i)CHECK(DROP(target,4096)==0);
    __atomic_store_n(&done,1,__ATOMIC_SEQ_CST);
    while(__atomic_load_n(&tid,__ATOMIC_SEQ_CST))__asm__ volatile("pause");
    CHECK(reads>0);
    CHECK(call(UNMAP,target,4096,0,0,0,0)==0);
    CHECK(call(WRITE,1,(word)"madvise-read-ok\n",16,0,0,0)==16);
    call(EXIT,42,0,0,0,0,0);__builtin_unreachable();
}
#else
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    const word page=4096, length=6*4096;
    word address=(word)call(MMAP,0,length,3,0x22,(word)-1,0);
    CHECK(address && address<(word)-4095);
    volatile unsigned char *p=(volatile unsigned char *)address;
    for(word i=0;i<length;++i)p[i]=0x39;
    CHECK(PROT(address,page,0)==0);
    CHECK(PROT(address+page,page,1)==0);
    CHECK(PROT(address+2*page,page,5)==0);
    CHECK(DROP(address,4*page)==0);
    /* Readonly and executable pages remain readable before any mprotect. */
    for(word i=page;i<4*page;++i)CHECK(p[i]==0);
    for(word i=4*page;i<length;++i)CHECK(p[i]==0x39);
    CHECK(PROT(address,4*page,3)==0);
    for(word i=0;i<4*page;++i)CHECK(p[i]==0);
    /* Non-page-sized lengths cover the entire final page, not its neighbors. */
    for(word i=0;i<length;++i)p[i]=0x57;
    CHECK(DROP(address+page,page+1)==0);
    for(word i=0;i<length;++i)CHECK(p[i]==(i>=page && i<3*page?0:0x57));
    CHECK(DROP(address+1,page)==-22);
    /* Linux reports an unmapped hole but still discards mapped neighbors. */
    for(word i=0;i<length;++i)p[i]=0x68;
    CHECK(call(UNMAP,address+2*page,page,0,0,0,0)==0);
    CHECK(DROP(address,length)==-12);
    for(word i=0;i<length;++i)if(i<2*page || i>=3*page)CHECK(p[i]==0);
    CHECK(call(UNMAP,address,length,0,0,0,0)==0);
    CHECK(DROP(address,0)==0);
    /* Initially PROT_NONE pages remain mapped and become zero when enabled. */
    address=(word)call(MMAP,0,3*page,0,0x22,(word)-1,0);
    CHECK(address && address<(word)-4095);
    CHECK(DROP(address,3*page)==0);
    CHECK(PROT(address,3*page,3)==0);
    p=(volatile unsigned char *)address;
    for(word i=0;i<3*page;++i)CHECK(p[i]==0);
    CHECK(call(UNMAP,address,3*page,0,0,0,0)==0);
    CHECK(call(WRITE,1,(word)"madvise-zero-ok\n",16,0,0,0)==16);
    call(EXIT,42,0,0,0,0,0);__builtin_unreachable();
}
#endif

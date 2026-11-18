/* Check actual memset through the packaged glibc, including boundary pages. */
typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ word;
#ifdef NR_INTERPOSE
void *memset(void *memory,int value,size_t size) {
    volatile unsigned char *p=memory;
    unsigned char byte=(unsigned char)value;
    for(size_t i=0;i<size;++i) p[i]=byte?byte:0x5b;
    return memory;
}
#else
extern void *mmap(void *,size_t,int,int,int,long);
extern int mprotect(void *,size_t,int);
extern int munmap(void *,size_t);
extern void *memset(void *,int,size_t);
extern long write(int,const void *,size_t);
extern long read(int,void *,size_t);
extern int open(const char *,int,...);
extern int close(int);
extern int __libc_start_main(int (*)(void),int,char **,void *,void *,void *,void *);
extern void _exit(int) __attribute__((noreturn));
#define CHECK(c) do {if(!(c)) _exit(__LINE__);} while(0)

static void stage(const char *name,size_t size) {
    CHECK(write(1,name,size)==(long)size);
    char byte;
    CHECK(read(0,&byte,1)==1);
}

static int test(void) {
    extern char *getenv(const char *);
    if(getenv("ZERO_INTERPOSE")) {
        unsigned char *p=mmap(0,131072,3,0x22,-1,0);
        CHECK((word)p<(word)-4095 && memset(p,0,131072)==p);
        CHECK(p[0]==0x5b && p[65536]==0x5b && p[131071]==0x5b);
        CHECK(munmap(p,131072)==0);
        CHECK(write(1,"interpose-ok\n",13)==13);
        return 42;
    }
    const size_t size=48*1024*1024;
    volatile unsigned char *p=mmap(0,size+8192,3,0x22,-1,0);
    CHECK((word)p<(word)-4095);
    /* Resolve lazy PLT and exercise a nonzero fill before checking optimization. */
    CHECK(memset((void *)p,0x37,size+8192)==(void *)p);
    stage("dirty\n",6);
    unsigned char *first=(unsigned char *)p+31;
    size_t length=size+103;
    CHECK(memset(first,256,length)==first); /* memset uses the low byte. */
    CHECK(p[30]==0x37 && first[0]==0 && first[length-1]==0 && first[length]==0x37);
    stage("zero\n",5); /* Measure resident memory before reading every zero page. */
    for(size_t i=0;i<length;++i) CHECK(first[i]==0);
    for(size_t i=0;i<31;++i) CHECK(p[i]==0x37);
    for(size_t i=31+length;i<size+8192;++i) CHECK(p[i]==0x37);
    CHECK(memset(first,-1,length)==first);
    for(size_t i=0;i<length;++i) CHECK(first[i]==255);
    int sink=open("/cache-sink",0x242,0600);
    CHECK(sink>=0 && write(sink,first+8192,1)==1); /* Warm validation cache. */
    CHECK(memset(first,0,length)==first);
    CHECK(first[0]==0 && first[length/2]==0 && first[length-1]==0);
    CHECK(p[30]==0x37 && first[length]==0x37);
    CHECK(write(sink,first+8192,1)==1);
    CHECK(mprotect((void *)(p+8192),4096,0)==0);
    CHECK(write(sink,first+8192,1)==-1); /* A real protection change must invalidate. */
    CHECK(mprotect((void *)(p+8192),4096,3)==0);
    CHECK(write(sink,first+8192,1)==1 && close(sink)==0);
    CHECK(mprotect((void *)p,size+8192,7)==0);
    CHECK(memset(first,0x49,length)==first);
    CHECK(memset(first,0,length)==first); /* Executable private pages use libc. */
    CHECK(first[0]==0 && first[length/2]==0 && first[length-1]==0);
    CHECK(munmap((void *)p,size+8192)==0);
    /* A shared mapping must update its aliases, never become private zeros. */
    int fd=open("/shared-zero",0x242,0600);
    CHECK(fd>=0);
    unsigned char block[4096]={0};
    for(unsigned i=0;i<32;++i) CHECK(write(fd,block,sizeof(block))==sizeof(block));
    unsigned char *a=mmap(0,131072,3,1,fd,0),*b=mmap(0,131072,3,1,fd,0);
    CHECK((word)a<(word)-4095 && (word)b<(word)-4095);
    CHECK(memset(a,0x72,131072)==a && b[65536]==0x72);
    CHECK(memset(a,0,131072)==a && b[65536]==0 && b[131071]==0);
    CHECK(memset(a,0x61,131072)==a);
    unsigned char *private=mmap(0,131072,3,2,fd,0);
    CHECK((word)private<(word)-4095 && private[65536]==0x61);
    CHECK(memset(private,0,131072)==private && private[65536]==0);
    CHECK(a[65536]==0x61 && b[131071]==0x61);
    CHECK(munmap(private,131072)==0);
    CHECK(munmap(a,131072)==0 && munmap(b,131072)==0 && close(fd)==0);
    CHECK(write(1,"zero-pages-ok\n",14)==14);
    return 42;
}

__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    __libc_start_main(test,0,0,0,0,0,0);
    __builtin_unreachable();
}
#endif

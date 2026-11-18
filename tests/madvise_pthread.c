/* Reclaim a real glibc pthread's user-owned stack immediately after join. */
typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ word;
extern void *mmap(void *,size_t,int,int,int,long);
extern int mprotect(void *,size_t,int);
extern int madvise(void *,size_t,int);
extern int munmap(void *,size_t);
extern int pthread_attr_init(void *);
extern int pthread_attr_setstack(void *,void *,size_t);
extern int pthread_attr_destroy(void *);
extern int pthread_create(word *,const void *,void *(*)(void *),void *);
extern int pthread_join(word,void **);
extern long write(int,const void *,size_t);
extern int __libc_start_main(int (*)(void),int,char **,void *,void *,void *,void *);
extern void _exit(int) __attribute__((noreturn));
#define CHECK(c) do {if(!(c))_exit(__LINE__);} while(0)

static void *worker(void *value) {
    volatile unsigned char buffer[8192];
    for(unsigned i=0;i<sizeof(buffer);++i)buffer[i]=(unsigned char)i;
    for(unsigned i=0;i<sizeof(buffer);++i)CHECK(buffer[i]==(unsigned char)i);
    return value;
}
static int test(void) {
    const size_t size=512*1024;
    for(word round=1;round<=24;++round) {
        unsigned char *stack=mmap(0,size,3,0x22,-1,0);
        CHECK((word)stack<(word)-4095);
        word attributes[16] __attribute__((aligned(16))),thread;
        CHECK(pthread_attr_init(attributes)==0);
        CHECK(pthread_attr_setstack(attributes,stack,size)==0);
        CHECK(pthread_create(&thread,attributes,worker,(void *)round)==0);
        void *value=0;
        CHECK(pthread_join(thread,&value)==0 && (word)value==round);
        CHECK(pthread_attr_destroy(attributes)==0);
        /* join promises that no thread still uses this stack or its TLS. */
        CHECK(madvise(stack,size,4)==0);
        for(size_t i=0;i<size;++i)CHECK(stack[i]==0);
        CHECK(mprotect(stack,size,0)==0);
        CHECK(munmap(stack,size)==0);
    }
    CHECK(write(1,"madvise-pthread-ok\n",19)==19);
    return 42;
}
__attribute__((noreturn,force_align_arg_pointer)) void _start(void) {
    __libc_start_main(test,0,0,0,0,0,0);__builtin_unreachable();
}

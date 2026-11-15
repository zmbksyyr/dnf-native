/* Exercise packaged glibc/libpthread robust mutex recovery, not a mock mutex. */
typedef __UINTPTR_TYPE__ word;
extern int pthread_create(word *, const void *, void *(*)(void *), void *);
extern int pthread_join(word, void **);
extern int pthread_mutexattr_init(void *);
extern int pthread_mutexattr_setrobust(void *, int);
extern int pthread_mutexattr_destroy(void *);
extern int pthread_mutex_init(void *, const void *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_consistent(void *);
extern int pthread_mutex_unlock(void *);
extern int pthread_mutex_destroy(void *);
extern int __libc_start_main(int (*)(void), int, char **, void *, void *, void *, void *);
extern void _exit(int) __attribute__((noreturn));

#define CHECK(c) do { if (!(c)) _exit(__LINE__); } while (0)
static word mutex[8];
static void *owner(void *unused) {
    (void)unused;
    CHECK(pthread_mutex_lock(mutex) == 0);
    return (void *)42; /* Thread exits holding the robust mutex. */
}
static int main_test(void) {
    int attributes[4];
    CHECK(pthread_mutexattr_init(attributes) == 0);
    CHECK(pthread_mutexattr_setrobust(attributes, 1) == 0);
    CHECK(pthread_mutex_init(mutex, attributes) == 0);
    CHECK(pthread_mutexattr_destroy(attributes) == 0);
    for (unsigned round = 0; round < 2; ++round) {
        word thread;
        void *value = 0;
        CHECK(pthread_create(&thread, 0, owner, 0) == 0);
        CHECK(pthread_join(thread, &value) == 0 && value == (void *)42);
        CHECK(pthread_mutex_lock(mutex) == 130); /* EOWNERDEAD */
        if (!round) CHECK(pthread_mutex_consistent(mutex) == 0);
        CHECK(pthread_mutex_unlock(mutex) == 0);
    }
    CHECK(pthread_mutex_lock(mutex) == 131); /* ENOTRECOVERABLE */
    CHECK(pthread_mutex_destroy(mutex) == 0);
    return 42;
}
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    __libc_start_main(main_test, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

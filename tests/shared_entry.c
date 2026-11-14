/* A normal DSO has no process entry. Its constructor, export and destructor
   still execute through the upstream Linux dynamic loader. */
#ifdef NR_SHARED
extern void entry_record(int value);
__attribute__((constructor)) static void init(void) { entry_record(1); }
__attribute__((destructor)) static void fini(void) { entry_record(3); }
int entry_value(int value) { entry_record(2); return value + 29; }
#else
extern void *dlopen(const char *path, int flags);
extern void *dlsym(void *handle, const char *name);
extern int dlclose(void *handle);
static volatile unsigned seen;
void entry_record(int value) { seen = seen * 10 + value; }
__attribute__((noreturn)) static void finish(int status) {
#ifdef __x86_64__
    __asm__ volatile("syscall" : : "a"(60), "D"(status) : "rcx", "r11", "memory");
#else
    __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
#endif
    __builtin_unreachable();
}
#define CHECK(value) do { if (!(value)) finish(__LINE__); } while (0)
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
#ifdef NR_INVALID_DSO
    CHECK(!dlopen("/entry-invalid.so", 2));
#else
    void *handle = dlopen("/entry-zero.so", 2);
    CHECK(handle && seen == 1);
    int (*function)(int) = (int (*)(int))dlsym(handle, "entry_value");
    CHECK(function && function(13) == 42 && seen == 12);
    CHECK(dlclose(handle) == 0 && seen == 123);
#endif
    finish(42);
}
#endif

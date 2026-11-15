#ifndef DNF_NATIVE_RUN_H
#define DNF_NATIVE_RUN_H

#include "elf.h"

/* The original game without DP2's allocator needs over 1024 live mappings. */
#define NR_VM_SLOTS 4096
#define NR_EVENTFD 32
typedef struct NrEventFd NrEventFd;

typedef struct {
    uint32_t events;
    uint64_t data;
    unsigned char used;
} NrWatch;

typedef struct {
    uintptr_t number, pc;
    unsigned count;
} NrGap;

typedef struct {
    uintptr_t tls_base;
    unsigned char *tls_code;
    uint64_t signal_mask, saved_mask;
    CONTEXT saved_context, initial_context;
    int in_signal;
    uintptr_t clear_tid, robust_head;
    DWORD tid;
    HANDLE handle;
    NrGap gaps[32];
} NrThread;

typedef struct {
    uintptr_t handler, flags, restorer;
    uint64_t mask;
} NrSignal;

typedef struct {
    void *base;
    size_t size;
    unsigned char *mapped_pages; /* NULL means the entire guest range is mapped. */
    HANDLE source;
    HANDLE section; /* Real shared file view; NULL for VirtualAlloc regions. */
    char *path; /* Guest pathname for /proc/self/maps, when this is a file image. */
    uint64_t file_offset, file_inode;
    NrElf image;
} NrVm;

typedef struct {
    NrElf program, interpreter;
    uintptr_t stack_low, stack_high;
    uintptr_t heap_base, heap_break;
    size_t heap_committed;
    uintptr_t auxv_address;
    size_t auxv_size;
    wchar_t root[32768];
    char cwd[4096];
    HANDLE files[256];
    unsigned char file_access[256];
    char *synthetic_files[256];
    size_t synthetic_sizes[256];
    uint64_t synthetic_offsets[256];
    unsigned char descriptor_flags[256];
    uint64_t directory_offset[256];
    unsigned char flock_mode[256];
    uintptr_t sockets[256];
    unsigned char nonblock[256];
    unsigned char connect_state[256]; /* 1: pending, 2: failed SO_ERROR consumed */
    uint64_t receive_timeout[256];
    uint64_t send_timeout[256];
    NrWatch *epolls[256];
    NrEventFd *eventfds[256];
    NrVm vm[NR_VM_SLOTS];
    unsigned vm_count; /* Highest slot that has been used; holes remain reusable. */
    CRITICAL_SECTION memory_lock;
    NrSignal actions[65];
    uint64_t signal_pending;
    HANDLE terminate_event, usr2_event;
    unsigned char *signal_code;
    NrThread threads[64];
    volatile LONG thread_count;
    int argc;
    char **argv;
    int environment_count;
    char *environment[32];
} NrRun;

int nr_run(NrRun *run);
int nr_memory(uintptr_t address, uintptr_t bytes, int write);
int nr_path(NrRun *run, const char *guest, wchar_t *path);
int nr_files(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
uint64_t nr_file_inode(HANDLE file);
int nr_vm(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
int nr_tls(NrRun *run, CONTEXT *context, const NrTrap *trap);
int nr_signals(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
int nr_deliver_signal(NrRun *run, CONTEXT *context);
int nr_net(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
NrThread *nr_current(void);
DWORD nr_thread_tls_slot(void);
int nr_thread_init(NrRun *run);
void nr_thread_attach(NrThread *thread);
int nr_threads(NrRun *run, uintptr_t number, const uintptr_t args[6], CONTEXT *context, intptr_t *result);
void nr_log_image(const NrElf *elf, const char *path);
void nr_log_gap(NrRun *run, uintptr_t number, const uintptr_t args[6], uintptr_t pc);
void nr_log_crash(NrRun *run, EXCEPTION_POINTERS *exception);
void nr_log_thread_exit(const NrThread *thread, uintptr_t status);
void nr_log_robust_failure(const NrThread *thread, uintptr_t address, const char *reason);
int nr_eventfd(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
intptr_t nr_eventfd_events(NrRun *run, unsigned fd, short requested);

#endif

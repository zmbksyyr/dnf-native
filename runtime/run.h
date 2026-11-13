#ifndef DNF_NATIVE_RUN_H
#define DNF_NATIVE_RUN_H

#include "elf.h"

#define NR_VM_SLOTS 1024

typedef struct {
    uint32_t events;
    uint64_t data;
    unsigned char used;
} NrWatch;

typedef struct {
    uintptr_t tls_base, tls_resume, tls_end;
    unsigned char *tls_code;
    uint64_t signal_mask, saved_mask;
    CONTEXT saved_context, initial_context;
    int in_signal;
    uintptr_t clear_tid, robust_head;
    DWORD tid;
    HANDLE handle;
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
    NrElf image;
} NrVm;

typedef struct {
    NrElf program, interpreter;
    uintptr_t stack_low, stack_high;
    uintptr_t heap_base, heap_break;
    size_t heap_committed;
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
    uint64_t receive_timeout[256];
    uint64_t send_timeout[256];
    NrWatch *epolls[256];
    NrVm vm[NR_VM_SLOTS];
    NrSignal actions[65];
    uint64_t signal_pending;
    HANDLE terminate_event;
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
int nr_vm(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
int nr_tls(NrRun *run, CONTEXT *context, const NrTrap *trap);
int nr_tls_finish(NrRun *run, CONTEXT *context);
int nr_signals(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
int nr_deliver_signal(NrRun *run, CONTEXT *context);
int nr_net(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result);
NrThread *nr_current(void);
int nr_thread_init(NrRun *run);
void nr_thread_attach(NrThread *thread);
int nr_threads(NrRun *run, uintptr_t number, const uintptr_t args[6], CONTEXT *context, intptr_t *result);

#endif

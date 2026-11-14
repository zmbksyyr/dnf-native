#include "run.h"
#include <string.h>

static DWORD thread_key = TLS_OUT_OF_INDEXES;
static CRITICAL_SECTION wait_lock;
typedef struct Waiter { struct Waiter *next; uintptr_t address; HANDLE event; } Waiter;
static Waiter *waiters;

NrThread *nr_current(void) { return thread_key == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(thread_key); }
void nr_thread_attach(NrThread *thread) {
    if (!thread->tid) thread->tid = GetCurrentThreadId();
    if (!TlsSetValue(thread_key, thread)) ExitProcess(124);
}
int nr_thread_init(NrRun *run) {
    (void)run;
    thread_key = TlsAlloc();
    InitializeCriticalSection(&wait_lock);
    return thread_key != TLS_OUT_OF_INDEXES;
}

/* Caller holds wait_lock while changing either a futex word or its queue. */
static intptr_t wake_locked(uintptr_t address, uintptr_t count) {
    intptr_t woken = 0;
    for (Waiter *w = waiters; w && count; w = w->next) {
        if (w->address != address) continue;
        if (WaitForSingleObject(w->event, 0) == WAIT_OBJECT_0) continue;
        SetEvent(w->event); --count; ++woken;
    }
    return woken;
}

static intptr_t wake(uintptr_t address, uintptr_t count) {
    EnterCriticalSection(&wait_lock);
    intptr_t woken = wake_locked(address, count);
    LeaveCriticalSection(&wait_lock);
    return woken;
}

static intptr_t requeue(const uintptr_t args[6]) {
    if ((args[4] & 3) || (int32_t)args[2] < 0 || (int32_t)args[3] < 0) return -22;
    if (!nr_memory(args[4], 4, 0)) return -14;
    EnterCriticalSection(&wait_lock);
    if (__atomic_load_n((const uint32_t *)args[0], __ATOMIC_SEQ_CST) != (uint32_t)args[5]) {
        LeaveCriticalSection(&wait_lock);
        return -11;
    }
    intptr_t affected = wake_locked(args[0], args[2]);
    uintptr_t count = args[3];
    for (Waiter *w = waiters; w && count; w = w->next) {
        if (w->address != args[0] || WaitForSingleObject(w->event, 0) == WAIT_OBJECT_0) continue;
        w->address = args[4];
        --count; ++affected;
    }
    LeaveCriticalSection(&wait_lock);
    return affected;
}

static intptr_t wake_op(const uintptr_t args[6]) {
    if ((args[4] & 3) || (int32_t)args[2] < 0 || (int32_t)args[3] < 0) return -22;
    if (!nr_memory(args[4], 4, 1)) return -14;
    uint32_t encoded = (uint32_t)args[5], operation = encoded >> 28, comparison = (encoded >> 24) & 15;
    int32_t operand = (int32_t)(encoded << 8) >> 20, compare = (int32_t)(encoded << 20) >> 20;
    if (operation & 8) {
        if (operand < 0 || operand > 31) return -38;
        operand = (int32_t)(1u << operand);
        operation &= 7;
    }
    if (operation > 4 || comparison > 5) return -38;
    EnterCriticalSection(&wait_lock);
    volatile LONG *word = (volatile LONG *)args[4];
    uint32_t old = (uint32_t)InterlockedCompareExchange(word, 0, 0), value;
    for (;;) {
        switch (operation) {
            case 0: value = (uint32_t)operand; break;
            case 1: value = old + (uint32_t)operand; break;
            case 2: value = old | (uint32_t)operand; break;
            case 3: value = old & ~(uint32_t)operand; break;
            default: value = old ^ (uint32_t)operand; break;
        }
        uint32_t actual = (uint32_t)InterlockedCompareExchange(word, (LONG)value, (LONG)old);
        if (actual == old) break;
        old = actual;
    }
    int32_t previous = (int32_t)old;
    int matched = comparison == 0 ? previous == compare : comparison == 1 ? previous != compare :
                  comparison == 2 ? previous < compare : comparison == 3 ? previous <= compare :
                  comparison == 4 ? previous > compare : previous >= compare;
    intptr_t woken = wake_locked(args[0], args[2]);
    if (matched) woken += wake_locked(args[4], args[3]);
    LeaveCriticalSection(&wait_lock);
    return woken;
}

static DWORD WINAPI child_entry(void *argument) {
    NrThread *thread = argument;
    nr_thread_attach(thread);
    /* New Windows thread keeps its own TEB and host TLS. Guest registers,
       Linux stack and logical TLS are restored independently. */
    RaiseException(0xe0444e46u, 0, 0, NULL);
    return 124;
}

int nr_threads(NrRun *run, uintptr_t number, const uintptr_t args[6], CONTEXT *c, intptr_t *result) {
#ifdef _WIN64
    enum {CLONE = 56, FUTEX = 202, SETTID = 218, EXIT = 60, ROBUST = 273};
#else
    enum {CLONE = 120, FUTEX = 240, SETTID = 258, EXIT = 1, ROBUST = 311};
#endif
    NrThread *thread = nr_current();
    if (number == SETTID) { thread->clear_tid = args[0]; *result = thread->tid; return 1; }
    if (number == ROBUST) {
        /* Robust owner-death handling is not implemented. */
        *result = -38; return 1;
    }
    if (number == EXIT) {
        if (thread->clear_tid && nr_memory(thread->clear_tid, 4, 1)) {
            InterlockedExchange((volatile LONG *)thread->clear_tid, 0);
            wake(thread->clear_tid, UINT32_MAX);
        }
        if (thread->tls_code) VirtualFree(thread->tls_code, 0, MEM_RELEASE);
        thread->tls_code = NULL;
        if (!InterlockedDecrement(&run->thread_count)) ExitProcess((DWORD)args[0] & 255);
        ExitThread((DWORD)args[0] & 255);
    }
    if (number == FUTEX) {
        uintptr_t operation = args[1] & ~128u;
        if (args[0] & 3) { *result = -22; return 1; }
        if (!nr_memory(args[0], 4, 0)) { *result = -14; return 1; }
        if (operation == 1) { *result = wake(args[0], args[2]); return 1; }
        if (operation == 4) { *result = requeue(args); return 1; }
        if (operation == 5) { *result = wake_op(args); return 1; }
        if (operation != 0 || (args[1] & ~129u)) { *result = -38; return 1; }
        DWORD timeout = INFINITE;
        if (args[3]) {
            if (!nr_memory(args[3], 2 * sizeof(uintptr_t), 0)) { *result = -14; return 1; }
            intptr_t duration[2]; memcpy(duration, (const void *)args[3], sizeof(duration));
            if (duration[0] < 0 || duration[1] < 0 || duration[1] >= 1000000000) { *result = -22; return 1; }
            uint64_t ms = (uint64_t)duration[0] * 1000 + ((uint64_t)duration[1] + 999999) / 1000000;
            if (ms >= INFINITE) { *result = -38; return 1; }
            timeout = (DWORD)ms;
        }
        Waiter waiter = {NULL, args[0], CreateEventW(NULL, TRUE, FALSE, NULL)};
        if (!waiter.event) { *result = -12; return 1; }
        EnterCriticalSection(&wait_lock);
        if ((uint32_t)InterlockedCompareExchange((volatile LONG *)args[0], 0, 0) != (uint32_t)args[2]) {
            LeaveCriticalSection(&wait_lock); CloseHandle(waiter.event); *result = -11; return 1;
        }
        waiter.next = waiters; waiters = &waiter;
        LeaveCriticalSection(&wait_lock);
        DWORD waited = WaitForSingleObject(waiter.event, timeout);
        EnterCriticalSection(&wait_lock);
        Waiter **link = &waiters;
        while (*link && *link != &waiter) link = &(*link)->next;
        if (*link) *link = waiter.next;
        LeaveCriticalSection(&wait_lock);
        CloseHandle(waiter.event);
        *result = waited == WAIT_OBJECT_0 ? 0 : waited == WAIT_TIMEOUT ? -110 : -5;
        return 1;
    }
    if (number != CLONE) return 0;
    /* Actual pthread clone contract observed in glibc. Process fork and other
       flag combinations remain unsupported instead of inventing a child. */
    if (args[0] != 0x3d0f00 || !args[1]) { *result = -38; return 1; }
    if (!nr_memory(args[1], 2 * sizeof(uintptr_t), 1) || !nr_memory(args[2], 4, 1)) { *result = -14; return 1; }
    NrThread *child = NULL;
    for (unsigned i = 1; i < 64; ++i) if (!run->threads[i].handle) { child = &run->threads[i]; break; }
    if (!child) { *result = -11; return 1; }
    child->initial_context = *c;
    child->signal_mask = thread->signal_mask;
#ifdef _WIN64
    child->tls_base = args[4]; child->clear_tid = args[3];
    child->initial_context.Rsp = args[1]; child->initial_context.Rax = 0;
    child->initial_context.Rip += 2; child->initial_context.Rcx = child->initial_context.Rip;
    child->initial_context.R11 = child->initial_context.EFlags & ~0x10000u;
#else
    if (!nr_memory(args[3], 16, 0)) { *result = -14; return 1; }
    uint32_t descriptor[4]; memcpy(descriptor, (const void *)args[3], sizeof(descriptor));
    if (descriptor[0] != 12 || descriptor[2] != 0xfffff || descriptor[3] != 0x51) { *result = -38; return 1; }
    child->tls_base = descriptor[1]; child->clear_tid = args[4];
    child->initial_context.Esp = args[1]; child->initial_context.Eax = 0; child->initial_context.Eip += 2;
#endif
    if (!nr_memory(child->tls_base, sizeof(uintptr_t), 1) || !nr_memory(child->clear_tid, 4, 1)) { *result = -14; return 1; }
    DWORD tid;
    child->handle = CreateThread(NULL, 1024 * 1024, child_entry, child, CREATE_SUSPENDED, &tid);
    if (!child->handle) { *result = -11; return 1; }
    child->tid = tid;
    memcpy((void *)args[2], &tid, 4);
    InterlockedIncrement(&run->thread_count);
    if (ResumeThread(child->handle) == (DWORD)-1) {
        TerminateThread(child->handle, 124); CloseHandle(child->handle); child->handle = NULL;
        InterlockedDecrement(&run->thread_count); *result = -11; return 1;
    }
    *result = tid;
    return 1;
}

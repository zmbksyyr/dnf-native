#include "run.h"
#include <string.h>

/* eventfd is used by the packaged Frida/glib path. Keep a real counter with
   separate readable/writable events; a pipe cannot preserve its ABI. */
struct NrEventFd {
    uint64_t value;
    unsigned references;
    int semaphore;
    HANDLE readable, writable;
};

static void update(NrEventFd *event) {
    if (event->value) SetEvent(event->readable); else ResetEvent(event->readable);
    if (event->value < UINT64_MAX - 1) SetEvent(event->writable); else ResetEvent(event->writable);
}

static void release(NrEventFd *event) {
    if (--event->references) return;
    CloseHandle(event->readable); CloseHandle(event->writable);
    HeapFree(GetProcessHeap(), 0, event);
}

intptr_t nr_eventfd_events(NrRun *run, unsigned fd, short requested) {
    EnterCriticalSection(&run->memory_lock);
    NrEventFd *event = fd < 256 ? run->eventfds[fd] : NULL;
    intptr_t result = !event ? 32 : (event->value ? requested & (1 | 64) : 0) |
                     (event->value < UINT64_MAX - 1 ? requested & (4 | 256) : 0);
    LeaveCriticalSection(&run->memory_lock);
    return result;
}

int nr_eventfd(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
#ifdef _WIN64
    enum {CREATE = 284, CREATE2 = 290, READ = 0, WRITE = 1, CLOSE = 3, FCNTL = 72};
#else
    enum {CREATE = 323, CREATE2 = 328, READ = 3, WRITE = 4, CLOSE = 6, FCNTL = 55};
    if (number == 221) number = FCNTL;
#endif
    if (number == CREATE || number == CREATE2) {
        uintptr_t flags = number == CREATE2 ? args[1] : 0;
        if (flags & ~(uintptr_t)0x80801) { *result = -22; return 1; }
        NrEventFd *event = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*event));
        if (!event) { *result = -12; return 1; }
        event->readable = CreateEventW(NULL, TRUE, FALSE, NULL);
        event->writable = CreateEventW(NULL, TRUE, TRUE, NULL);
        event->references = 1;
        if (!event->readable || !event->writable) { release(event); *result = -12; return 1; }
        event->value = (uint32_t)args[0]; event->semaphore = (flags & 1) != 0;
        update(event);
        EnterCriticalSection(&run->memory_lock);
        unsigned fd;
        for (fd = 0; fd < 256 && run->file_access[fd]; ++fd) {}
        if (fd == 256) { release(event); *result = -24; }
        else {
            run->eventfds[fd] = event;
            run->nonblock[fd] = (flags & 0x800) != 0;
            run->descriptor_flags[fd] = (flags & 0x80000) != 0;
            run->file_access[fd] = NR_EVENTFD;
            *result = fd;
        }
        LeaveCriticalSection(&run->memory_lock);
        return 1;
    }
    if (args[0] >= 256) return 0;
    unsigned fd = (unsigned)args[0];
    EnterCriticalSection(&run->memory_lock);
    NrEventFd *event = run->eventfds[fd];
    if (!event) { LeaveCriticalSection(&run->memory_lock); return 0; }
    if (number == CLOSE) {
        run->eventfds[fd] = NULL;
        run->nonblock[fd] = run->descriptor_flags[fd] = 0;
        run->file_access[fd] = 0;
        release(event); *result = 0;
    } else if (number == FCNTL) {
        if (args[1] == 1) *result = run->descriptor_flags[fd];
        else if (args[1] == 2 && !(args[2] & ~(uintptr_t)1)) { run->descriptor_flags[fd] = (unsigned char)args[2]; *result = 0; }
        else if (args[1] == 3) *result = 2 | (run->nonblock[fd] ? 0x800 : 0);
        else if (args[1] == 4 && !(args[2] & ~(uintptr_t)(2 | 0x800))) { run->nonblock[fd] = (args[2] & 0x800) != 0; *result = 0; }
        else *result = -38;
    } else if (number == READ || number == WRITE) {
        int writing = number == WRITE, nonblock = run->nonblock[fd];
        if ((writing && args[2] != 8) || (!writing && args[2] < 8)) *result = -22;
        else if (!nr_memory(args[1], 8, !writing)) *result = -14;
        else {
            uint64_t value = 0;
            if (writing) memcpy(&value, (const void *)args[1], 8);
            if (writing && value == UINT64_MAX) *result = -22;
            else {
                ++event->references; /* A close cannot free a blocked operation. */
                for (;;) {
                    int ready = writing ? value <= UINT64_MAX - 1 - event->value : event->value != 0;
                    if (ready) {
                        if (!writing && !nr_memory(args[1], 8, 1)) { *result = -14; break; }
                        if (writing) event->value += value;
                        else {
                            value = event->semaphore ? 1 : event->value;
                            event->value -= value;
                            memcpy((void *)args[1], &value, 8);
                        }
                        update(event); *result = 8; break;
                    }
                    if (nonblock) { *result = -11; break; }
                    HANDLE events[] = {run->terminate_event, run->usr2_event, writing ? event->writable : event->readable};
                    LeaveCriticalSection(&run->memory_lock);
                    DWORD waited = WaitForMultipleObjects(3, events, FALSE, INFINITE);
                    EnterCriticalSection(&run->memory_lock);
                    if (waited == WAIT_OBJECT_0 + 2) {
                        /* A large write may still not fit while POLLOUT is
                           true. Wait briefly for a reader to lower the count. */
                        if (writing && value > UINT64_MAX - 1 - event->value) {
                            LeaveCriticalSection(&run->memory_lock); Sleep(1); EnterCriticalSection(&run->memory_lock);
                        }
                        continue;
                    }
                    if (waited == WAIT_OBJECT_0 || waited == WAIT_OBJECT_0 + 1) {
                        run->signal_pending |= 1ULL << (waited == WAIT_OBJECT_0 ? 14 : 11);
                        *result = -4;
                    } else *result = -5;
                    break;
                }
                release(event);
            }
        }
    } else { LeaveCriticalSection(&run->memory_lock); return 0; }
    LeaveCriticalSection(&run->memory_lock);
    return 1;
}

#include "run.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN64
enum {ACTION = 13, MASK = 14, KILL = 62};
#define PC(c) ((c)->Rip)
#define SP(c) ((c)->Rsp)
#else
enum {ACTION = 174, MASK = 175, KILL = 37};
#define PC(c) ((c)->Eip)
#define SP(c) ((c)->Esp)
#endif

int nr_signals(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
    NrThread *thread = nr_current();
    uintptr_t a = args[0], b = args[1], c = args[2];
    if (number == ACTION) {
        if (args[3] != 8 || !a || a > 64 || (b && (a == 9 || a == 19))) { *result = -22; return 1; }
        size_t bytes = 3 * sizeof(uintptr_t) + 8;
        if ((b && !nr_memory(b, bytes, 0)) || (c && !nr_memory(c, bytes, 1))) { *result = -14; return 1; }
        NrSignal action = {0};
        if (b) {
            memcpy(&action, (const void *)b, 3 * sizeof(uintptr_t));
            memcpy(&action.mask, (const void *)(b + 3 * sizeof(uintptr_t)), 8);
            /* SA_SIGINFO needs Linux siginfo/ucontext frames, which this
               initial one-thread handler path does not yet provide. */
            if (action.flags & ~(uintptr_t)(0x04000000 | 0x10000000 | 0x80000000u)) {
                *result = -38; return 1;
            }
        }
        if (c) {
            memcpy((void *)c, &run->actions[a], 3 * sizeof(uintptr_t));
            memcpy((void *)(c + 3 * sizeof(uintptr_t)), &run->actions[a].mask, 8);
        }
        if (b) run->actions[a] = action;
        *result = 0; return 1;
    }
    if (number == MASK) {
        if (args[3] != 8 || (b && a > 2)) { *result = -22; return 1; }
        if ((b && !nr_memory(b, 8, 0)) || (c && !nr_memory(c, 8, 1))) { *result = -14; return 1; }
        uint64_t value = 0;
        if (b) memcpy(&value, (const void *)b, 8);
        if (c) memcpy((void *)c, &thread->signal_mask, 8);
        if (b) {
            if (!a) thread->signal_mask |= value;
            else if (a == 1) thread->signal_mask &= ~value;
            else thread->signal_mask = value;
            thread->signal_mask &= ~((1ULL << 8) | (1ULL << 18));
        }
        *result = 0; return 1;
    }
    if (number == KILL) {
        if (b > 64) { *result = -22; return 1; }
        if (a != GetCurrentProcessId()) {
            if ((intptr_t)a <= 0 || (b != 0 && b != 15)) { *result = -38; return 1; }
            wchar_t name[80];
            swprintf(name, 80, L"Local\\dnf-native-terminate-%lu", (DWORD)a);
            HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
            if (!event) { *result = GetLastError() == ERROR_ACCESS_DENIED ? -1 : -3; return 1; }
            *result = !b || SetEvent(event) ? 0 : -5;
            CloseHandle(event);
            return 1;
        }
        if (b) {
            if (b == 19 || b == 18) { *result = -38; return 1; }
            run->signal_pending |= 1ULL << (b - 1);
        }
        *result = 0; return 1;
    }
    return 0;
}

int nr_deliver_signal(NrRun *run, CONTEXT *c) {
    NrThread *thread = nr_current();
    if (WaitForSingleObject(run->terminate_event, 0) == WAIT_OBJECT_0) run->signal_pending |= 1ULL << 14;
    if (thread->in_signal || thread->tls_resume) return 1;
    uint64_t ready = run->signal_pending & ~thread->signal_mask;
    for (unsigned signal = 1; signal <= 64; ++signal) {
        uint64_t bit = 1ULL << (signal - 1);
        if (!(ready & bit)) continue;
        run->signal_pending &= ~bit;
        NrSignal action = run->actions[signal];
        if (action.handler == 1 || (!action.handler && (signal == 17 || signal == 23 || signal == 28))) continue;
        if (!action.handler) ExitProcess(128 + signal);
        if (!nr_memory(action.handler, 1, 0) || !run->signal_code) return 0;
        uintptr_t stack = SP(c);
        uintptr_t restorer = (action.flags & 0x04000000) && action.restorer ? action.restorer : (uintptr_t)run->signal_code;
#ifdef _WIN64
        stack = ((stack - 128 - 8) & ~(uintptr_t)15) - 8;
        uintptr_t frame[] = {restorer};
#else
        stack = ((stack - 16) & ~(uintptr_t)15) - 4;
        uintptr_t frame[] = {restorer, signal};
#endif
        if (!nr_memory(stack, sizeof(frame), 1)) return 0;
        thread->saved_context = *c;
        thread->saved_mask = thread->signal_mask;
        thread->signal_mask |= action.mask;
        if (!(action.flags & 0x40000000)) thread->signal_mask |= bit;
        if (action.flags & 0x80000000u) memset(&run->actions[signal], 0, sizeof(NrSignal));
        memcpy((void *)stack, frame, sizeof(frame));
        SP(c) = stack;
        PC(c) = action.handler;
#ifdef _WIN64
        c->Rdi = signal;
#endif
        thread->in_signal = 1;
        return 1;
    }
    return 1;
}

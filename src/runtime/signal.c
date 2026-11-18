#include "run.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN64
enum {ACTION = 13, MASK = 14, KILL = 62, TGKILL = 234};
#define PC(c) ((c)->Rip)
#define SP(c) ((c)->Rsp)
#else
enum {ACTION = 174, MASK = 175, KILL = 37, TGKILL = 270};
#define PC(c) ((c)->Eip)
#define SP(c) ((c)->Esp)
#endif

enum { SA_SIGINFO = 4, SA_RESTORER = 0x04000000, SA_RESTART = 0x10000000,
       SA_NODEFER = 0x40000000, SA_RESETHAND = 0x80000000u };
void nr_signal_enter(void);

/* Every delivered handler owns its interrupted context. Unmasked signals can
   nest; sigreturn must restore the innermost frame before the outer one. */
struct NrSignalState {
    NrSignalState *previous;
    CONTEXT context;
    uint64_t mask;
    uintptr_t frame;
    int info;
};

/* Linux UAPI sigcontext/ucontext layouts, independent of Windows CONTEXT.
   Floating-point state stays in the saved native context; fpstate=NULL
   makes that current limitation visible to handlers. */
#ifdef _WIN64
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, flags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
} NrSigContext;
#else
typedef struct {
    uint32_t gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, flags, esp_at_signal, ss, fpstate, oldmask, cr2;
} NrSigContext;
#endif
typedef struct {
    uintptr_t flags, link, stack;
    uint32_t stack_flags;
#ifdef _WIN64
    uint32_t pad;
#endif
    uintptr_t stack_size;
    NrSigContext machine;
    unsigned char mask[8];
} NrUserContext;
typedef struct {
    uintptr_t restorer;
#ifndef _WIN64
    uintptr_t signal, info_address, context_address;
    unsigned char info[128];
#endif
    NrUserContext context;
#ifdef _WIN64
    unsigned char info[128];
#endif
} NrSignalFrame;
_Static_assert(sizeof(NrSignalFrame) <= sizeof(((NrThread *)0)->signal_frame_data), "signal staging size");
_Static_assert(offsetof(NrUserContext, machine) == (sizeof(void *) == 8 ? 40 : 20), "Linux ucontext mcontext");
_Static_assert(offsetof(NrUserContext, mask) == (sizeof(void *) == 8 ? 296 : 108), "Linux ucontext sigmask");

static void save_machine(NrSigContext *s, const CONTEXT *c, NrThread *thread) {
#ifdef _WIN64
    s->r8=c->R8; s->r9=c->R9; s->r10=c->R10; s->r11=c->R11;
    s->r12=c->R12; s->r13=c->R13; s->r14=c->R14; s->r15=c->R15;
    s->rdi=c->Rdi; s->rsi=c->Rsi; s->rbp=c->Rbp; s->rbx=c->Rbx;
    s->rdx=c->Rdx; s->rax=c->Rax; s->rcx=c->Rcx; s->rsp=c->Rsp; s->rip=c->Rip;
    s->cs=c->SegCs; s->ss=c->SegSs; s->fs=c->SegFs; s->gs=c->SegGs;
#else
    s->edi=c->Edi; s->esi=c->Esi; s->ebp=c->Ebp; s->esp=c->Esp; s->esp_at_signal=c->Esp;
    s->ebx=c->Ebx; s->edx=c->Edx; s->ecx=c->Ecx; s->eax=c->Eax; s->eip=c->Eip;
    s->cs=c->SegCs; s->ss=c->SegSs; s->ds=c->SegDs; s->es=c->SegEs; s->fs=c->SegFs;
    s->gs=thread->tls_base ? 12 * 8 + 3 : 0;
#endif
    s->flags=c->EFlags;
    s->oldmask=(uintptr_t)thread->signal_mask;
}

static void restore_machine(CONTEXT *c, const NrSigContext *s) {
#ifdef _WIN64
    c->R8=s->r8; c->R9=s->r9; c->R10=s->r10; c->R11=s->r11;
    c->R12=s->r12; c->R13=s->r13; c->R14=s->r14; c->R15=s->r15;
    c->Rdi=s->rdi; c->Rsi=s->rsi; c->Rbp=s->rbp; c->Rbx=s->rbx;
    c->Rdx=s->rdx; c->Rax=s->rax; c->Rcx=s->rcx; c->Rsp=s->rsp; c->Rip=s->rip;
#else
    c->Edi=s->edi; c->Esi=s->esi; c->Ebp=s->ebp; c->Esp=s->esp;
    c->Ebx=s->ebx; c->Edx=s->edx; c->Ecx=s->ecx; c->Eax=s->eax; c->Eip=s->eip;
#endif
    /* Only user arithmetic/control flags can change. Keep Windows selectors
       and host TLS; guest TLS remains the runtime's logical FS/GS base. */
    c->EFlags=(c->EFlags & ~0x40dd5u) | (s->flags & 0x40dd5u);
}

int nr_restore_signal(NrRun *run, CONTEXT *c) {
    NrThread *thread=nr_current();
    NrSignalState *state=thread->signal_state;
    if (!state) return 0;
    CONTEXT restored=state->context;
    uint64_t mask=state->mask;
    if (state->info) {
        NrSignalFrame frame;
        EnterCriticalSection(&run->memory_lock);
        int valid=nr_memory(state->frame, sizeof(frame), 0);
        if (valid) memcpy(&frame, (void *)state->frame, sizeof(frame));
        LeaveCriticalSection(&run->memory_lock);
        if (!valid || frame.context.machine.fpstate) return 0;
        restore_machine(&restored, &frame.context.machine);
        memcpy(&mask, frame.context.mask, 8);
        if (!nr_memory(PC(&restored), 1, 0) || !nr_memory(SP(&restored), 1, 0)) return 0;
    }
    *c=restored;
    thread->signal_mask=mask & ~((1ULL<<8) | (1ULL<<18));
    thread->signal_state=state->previous;
    HeapFree(GetProcessHeap(), 0, state);
    return 1;
}

int nr_signals(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
    NrThread *thread = nr_current();
    uintptr_t a = args[0], b = args[1], c = args[2];
    if (number == TGKILL) {
        if (!a || !b || a != GetCurrentProcessId() || b != thread->tid) { *result = -3; return 1; }
        if (c > 64) { *result = -22; return 1; }
        if (c) {
            run->signal_sender[c]=(int32_t)GetCurrentProcessId(); run->signal_origin[c]=-6;
            run->signal_pending |= 1ULL << (c - 1);
        }
        *result = 0; return 1;
    }
    if (number == ACTION) {
        if (args[3] != 8 || !a || a > 64 || (b && (a == 9 || a == 19))) { *result = -22; return 1; }
        size_t bytes = 3 * sizeof(uintptr_t) + 8;
        if ((b && !nr_memory(b, bytes, 0)) || (c && !nr_memory(c, bytes, 1))) { *result = -14; return 1; }
        NrSignal action = {0};
        if (b) {
            memcpy(&action, (const void *)b, 3 * sizeof(uintptr_t));
            memcpy(&action.mask, (const void *)(b + 3 * sizeof(uintptr_t)), 8);
            if (action.flags & ~(uintptr_t)(SA_SIGINFO | SA_RESTORER | SA_RESTART | SA_NODEFER | SA_RESETHAND)) {
                fprintf(stderr, "sigaction unsupported signal=%llu flags=0x%llx handler=0x%llx\n",
                        (unsigned long long)a, (unsigned long long)action.flags, (unsigned long long)action.handler);
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
            if ((intptr_t)a <= 0 || (b != 0 && b != 12 && b != 15)) { *result = -38; return 1; }
            wchar_t name[80];
            swprintf(name, 80, b == 12 ? L"Local\\dnf-native-sigusr2-%lu" : L"Local\\dnf-native-terminate-%lu", (DWORD)a);
            HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
            if (!event) { *result = GetLastError() == ERROR_ACCESS_DENIED ? -1 : -3; return 1; }
            *result = !b || SetEvent(event) ? 0 : -5;
            CloseHandle(event);
            return 1;
        }
        if (b) {
            if (b == 19 || b == 18) { *result = -38; return 1; }
            run->signal_sender[b]=(int32_t)GetCurrentProcessId(); run->signal_origin[b]=0;
            run->signal_pending |= 1ULL << (b - 1);
        }
        *result = 0; return 1;
    }
    return 0;
}

static int deliver_action(NrRun *run, CONTEXT *c, unsigned signal, const EXCEPTION_RECORD *fault) {
    NrThread *thread=nr_current();
    uint64_t bit=1ULL<<(signal-1);
    NrSignal action=run->actions[signal];
    if (!nr_memory(action.handler, 1, 0) || !run->signal_code) return 0;
    int info=(action.flags & SA_SIGINFO) != 0;
    size_t bytes=info ? sizeof(NrSignalFrame) : 2*sizeof(uintptr_t);
    uintptr_t stack=SP(c);
    size_t redzone=sizeof(void *)==8 ? 128 : 0;
    if (stack < bytes + redzone + 16) return 0;
    stack=((stack-bytes-redzone)&~(uintptr_t)15)-sizeof(uintptr_t);
    if (!nr_memory(stack,bytes,1)) return 0;
    uintptr_t restorer=(action.flags & SA_RESTORER) && action.restorer ? action.restorer : (uintptr_t)run->signal_code;
    NrSignalFrame *frame=(NrSignalFrame *)thread->signal_frame_data;
    memset(frame,0,sizeof(*frame));
    frame->restorer=restorer;
    uintptr_t info_address=stack+offsetof(NrSignalFrame,info);
    uintptr_t context_address=stack+offsetof(NrSignalFrame,context);
#ifndef _WIN64
    frame->signal=signal;
    frame->info_address=info_address;
    frame->context_address=context_address;
#endif
    if (info) {
        int32_t signo=(int32_t)signal, code=fault ? 1 : run->signal_origin[signal];
        int32_t pid=run->signal_sender[signal], uid=code==128 ? 0 : 1000;
        memcpy(frame->info,&signo,4);
        memcpy(frame->info+8,&code,4);
        size_t payload=sizeof(void *)==8 ? 16 : 12;
        if (fault) {
            uintptr_t address=(uintptr_t)fault->ExceptionInformation[1];
            MEMORY_BASIC_INFORMATION region;
            if (VirtualQuery((void *)address,&region,sizeof(region)) && region.State==MEM_COMMIT) code=2;
            memcpy(frame->info+8,&code,4);
            memcpy(frame->info+payload,&address,sizeof(address));
        } else {
            memcpy(frame->info+payload,&pid,4);
            memcpy(frame->info+payload+4,&uid,4);
        }
        frame->context.stack_flags=2; /* SS_DISABLE; no alternate stack. */
        save_machine(&frame->context.machine,c,thread);
        if (fault) {
            frame->context.machine.trapno=14;
            frame->context.machine.cr2=(uintptr_t)fault->ExceptionInformation[1];
            frame->context.machine.err=4 | (code==2 ? 1 : 0) |
                (fault->ExceptionInformation[0]==1 ? 2 : 0) |
                (fault->ExceptionInformation[0]==8 ? 16 : 0);
        }
        memcpy(frame->context.mask,&thread->signal_mask,8);
    }
    NrSignalState *state=HeapAlloc(GetProcessHeap(), 0, sizeof(*state));
    if (!state) return 0;
    state->context=*c;
    state->mask=thread->signal_mask;
    state->info=info;
    state->frame=stack;
    state->previous=thread->signal_state;
    thread->signal_state=state;
    thread->signal_mask |= action.mask;
    if (!(action.flags & SA_NODEFER)) thread->signal_mask |= bit;
    thread->signal_mask &= ~((1ULL<<8) | (1ULL<<18));
    if (action.flags & SA_RESETHAND) memset(&run->actions[signal], 0, sizeof(NrSignal));
    SP(c) = stack;
    PC(c) = (uintptr_t)nr_signal_enter;
#ifdef _WIN64
    c->Rax=action.handler; c->Rdi=stack; c->Rsi=(uintptr_t)frame; c->Rcx=bytes;
    c->Rdx=signal; c->R8=info_address; c->R9=context_address;
#else
    c->Eax=action.handler; c->Edi=stack; c->Esi=(uintptr_t)frame; c->Ecx=(DWORD)bytes;
#endif
    return 1;
}

int nr_deliver_signal(NrRun *run, CONTEXT *c) {
    NrThread *thread = nr_current();
    if (WaitForSingleObject(run->terminate_event, 0) == WAIT_OBJECT_0) {
        run->signal_origin[15]=128; run->signal_sender[15]=0; run->signal_pending |= 1ULL << 14;
    }
    if (WaitForSingleObject(run->usr2_event, 0) == WAIT_OBJECT_0) {
        run->signal_origin[12]=128; run->signal_sender[12]=0; run->signal_pending |= 1ULL << 11;
    }
    uint64_t ready = run->signal_pending & ~thread->signal_mask;
    for (unsigned signal = 1; signal <= 64; ++signal) {
        uint64_t bit = 1ULL << (signal - 1);
        if (!(ready & bit)) continue;
        /* External process signals prefer the guest group leader, as on
           Linux. Letting the busiest worker consume a controller stop can
           reenter its logger from the original game's shutdown handler. A
           blocked leader still leaves other unblocked threads eligible. */
        if (run->signal_origin[signal] == 128 && thread != &run->threads[0] &&
            !(run->threads[0].signal_mask & bit)) continue;
        run->signal_pending &= ~bit;
        NrSignal action = run->actions[signal];
        if (action.handler == 1 || (!action.handler && (signal == 17 || signal == 23 || signal == 28))) continue;
        if (!action.handler) ExitProcess(128 + signal);
        return deliver_action(run,c,signal,NULL);
    }
    return 1;
}

/* Synchronous memory faults belong to the interrupted guest thread, never the
   process-wide pending queue. A handler may repair memory or edit ucontext to
   resume; missing/blocked handlers retain the normal crash diagnostic path. */
int nr_deliver_fault(NrRun *run, CONTEXT *c, const EXCEPTION_RECORD *fault) {
    if (fault->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || fault->NumberParameters<2) return 0;
    const unsigned signal=11;
    NrThread *thread=nr_current();
    if (run->actions[signal].handler<=1 || (thread->signal_mask & (1ULL<<(signal-1)))) return 0;
    return deliver_action(run,c,signal,fault);
}

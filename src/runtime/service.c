#include "run.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifndef _WIN64
static void service_log(const char *format, ...) {
    /* Thread-exit diagnostics use kernel writes. Emit this line in one write
       too, so a concurrent exit cannot split a join result in the middle. */
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &written, NULL);
}

/* Independently derived from the fixed original ELF instructions and vtables.
   Free deletes shared handlers before its TCP workers have exited. Quiesce the
   three network workers first, then resume every original cleanup instruction.
   This is an application correction, not part of the Linux syscall contract. */
typedef struct {
    const char *sha256;
    uintptr_t pc, message, join, net_offset, udp_offset;
    uintptr_t accept_vtable, tcp_vtable, udp_vtable;
    int blocking_udp;
} ServiceStop;

static const ServiceStop profiles[] = {
    {"0979ef164615e5e3eae006fde74ca2a39a920367dae6a143847dbc0a8d49a967",
     0x806c0c2, 0x81aaacd, 0x8063692, 0x128, 0x9c, 0x81a67d8, 0x81a7180, 0x81a8940, 1},
    {"74b3adba3c2d376547ee65c53940129bd20af9e48926be3f4facc7d729dd65e3",
     0x805e3b4, 0x8156e1d, 0x805d9ba, 0xcc, 0xc8, 0x8155f78, 0x8156920, 0x815f6c0, 0},
};

static const ServiceStop *profile(const NrElf *elf) {
    for (unsigned i = 0; i < sizeof(profiles) / sizeof(*profiles); ++i)
        if (!strcmp(elf->sha256, profiles[i].sha256)) return &profiles[i];
    return NULL;
}

static int failed(const char *reason, uintptr_t object) {
    service_log("service shutdown failed object=0x%llx reason=%s\n", (unsigned long long)object, reason);
    return 0;
}

typedef struct {
    uintptr_t object;
    unsigned flag;
    NrThread *thread;
} Worker;

static int worker(NrRun *run, Worker *w, uintptr_t address, uintptr_t vtable, unsigned flag) {
    if (!nr_memory(address, 4, 0)) return failed("unreadable worker pointer", address);
    memcpy(&w->object, (const void *)address, 4);
    w->flag = flag;
    if (!w->object) return 1; /* Initialization may have failed before allocating a worker. */
    if (!nr_memory(w->object, flag + 1, 1) || *(const uintptr_t *)w->object != vtable)
        return failed("worker layout differs", w->object);
    uintptr_t pthread = *(const uintptr_t *)(w->object + 4);
    if (!pthread) return 1; /* No successful pthread_create. */
    for (unsigned i = 1; i < 64; ++i)
        if (run->threads[i].handle && run->threads[i].tls_base == pthread) {
            w->thread = &run->threads[i];
            return 1;
        }
    return failed("guest pthread has no runtime thread", w->object);
}
#endif

int nr_service_adapt(NrElf *elf, const char **error) {
#ifdef _WIN64
    (void)elf; (void)error;
#else
    const ServiceStop *p = profile(elf);
    if (!p) return 1;
    /* ET_EXEC only. Verify the entire overwritten MOV, plus original join body,
       against both the read-only input mapping and the execution mapping. */
    unsigned char expected[] = {0xc7, 0x04, 0x24, 0, 0, 0, 0};
    memcpy(expected + 3, &p->message, 4);
    const unsigned char join[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x08, 0x8b, 0x40, 0x04};
    uintptr_t sites[] = {p->pc, p->join};
    const void *bytes[] = {expected, join};
    size_t lengths[] = {sizeof(expected), sizeof(join)};
    if (elf->bias) { *error = "service shutdown correction requires original ET_EXEC addresses"; return 0; }
    for (unsigned n = 0; n < 2; ++n) {
        int valid = 0;
        for (unsigned i = 0; i < elf->header->phnum; ++i) {
            const NrElfProgram *segment = &elf->programs[i];
            if (segment->type != 1 || sites[n] < segment->vaddr) continue;
            size_t off = sites[n] - segment->vaddr;
            if (off > segment->filesz || lengths[n] > segment->filesz - off ||
                segment->offset > elf->file_size || off > elf->file_size - segment->offset ||
                lengths[n] > elf->file_size - segment->offset - off) continue;
            valid = !memcmp(elf->file + segment->offset + off, bytes[n], lengths[n]) &&
                    !memcmp((const void *)sites[n], bytes[n], lengths[n]);
            break;
        }
        if (!valid) { *error = "service shutdown correction original bytes differ"; return 0; }
    }
    DWORD old, ignored;
    if (!VirtualProtect((void *)p->pc, 2, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)p->pc, "\x0f\x0b", 2);
    if (!VirtualProtect((void *)p->pc, 2, old, &ignored)) return 0;
    elf->service_stop_pc = p->pc;
    service_log("service shutdown correction sha256=%s pc=0x%llx original=c70424%02x%02x%02x%02x replacement=0f0b scope=execution-memory\n",
            elf->sha256, (unsigned long long)p->pc, expected[3], expected[4], expected[5], expected[6]);
#endif
    return 1;
}

int nr_service_stop(NrRun *run, CONTEXT *context) {
#ifdef _WIN64
    (void)run; (void)context;
    return 0;
#else
    const ServiceStop *p = profile(&run->program);
    if (!p || context->Eip != p->pc || !nr_memory(context->Ebp + 8, 4, 0) || !nr_memory(context->Esp, 4, 1))
        return failed("invalid Free context", context->Eip);
    uintptr_t app = *(const uintptr_t *)(context->Ebp + 8), net = app + p->net_offset;
    Worker workers[3] = {{0}};
    if (!worker(run, &workers[0], net + 0x118, p->accept_vtable, 8) ||
        !worker(run, &workers[1], net + 4, p->tcp_vtable, 0x2c) ||
        !worker(run, &workers[2], app + p->udp_offset, p->udp_vtable, 8)) return 0;
    /* Original dispatches initialize their flags after a startup sleep. Wait
       for initialization (or actual thread exit) so an early stop is not lost. */
    ULONGLONG deadline = GetTickCount64() + 15000;
    for (unsigned i = 0; i < 3; ++i) {
        Worker *w = &workers[i];
        if (!w->thread) continue;
        if (!*(volatile unsigned char *)(w->object + w->flag))
            service_log("service shutdown await initialization worker=%u object=0x%llx\n",
                    i, (unsigned long long)w->object);
        while (!*(volatile unsigned char *)(w->object + w->flag) &&
               WaitForSingleObject(w->thread->handle, 0) == WAIT_TIMEOUT) {
            if (GetTickCount64() >= deadline) return failed("worker initialization timed out", w->object);
            Sleep(1);
        }
        *(volatile unsigned char *)(w->object + w->flag) = 0;
        service_log("service shutdown stop worker=%u object=0x%llx tid=%lu flag=0x%x\n",
                i, (unsigned long long)w->object, w->thread->tid, w->flag);
    }
    /* DBMW blocks in recvfrom. Closing its socket cancels that receive while
       the handler and receive buffer remain alive. No fabricated UDP packet.
       Manager instead uses a nonblocking socket and a one-second select. */
    Worker *udp = &workers[2];
    if (p->blocking_udp && udp->thread && WaitForSingleObject(udp->thread->handle, 0) == WAIT_TIMEOUT) {
        uintptr_t address = udp->object + 0x10;
        if (!nr_memory(address, 4, 0)) return failed("unreadable UDP handler", udp->object);
        uintptr_t handler = *(const uintptr_t *)address;
        if (!nr_memory(handler, 4, 1)) return failed("unreadable UDP socket", handler);
        uintptr_t args[6] = {*(const uint32_t *)handler};
        intptr_t result;
        if (!nr_net(run, 6, args, &result, 0) || result) return failed("UDP receive cancellation failed", handler);
        *(uint32_t *)handler = UINT32_MAX;
    }
    for (unsigned i = 0; i < 3; ++i) {
        Worker *w = &workers[i];
        if (!w->thread) continue;
        /* The OS wait bounds failure, but does not replace guest pthread_join:
           guest glibc must still reclaim its thread state and stack. */
        if (WaitForSingleObject(w->thread->handle, 15000) != WAIT_OBJECT_0)
            return failed("worker exit timed out", w->object);
        int (__cdecl *join)(uintptr_t) = (int (__cdecl *)(uintptr_t))p->join;
        int result = join(w->object);
        service_log("service shutdown joined worker=%u object=0x%llx tid=%lu result=%d\n",
                i, (unsigned long long)w->object, w->thread->tid, result);
        if (result) return failed("guest pthread_join failed", w->object);
    }
    /* Execute the original MOV [esp],message and continue at its original puts. */
    memcpy((void *)context->Esp, &p->message, 4);
    context->Eip += 7;
    service_log("service shutdown workers quiesced; resuming original Free\n");
    return 1;
#endif
}

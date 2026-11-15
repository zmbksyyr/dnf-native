#include "run.h"
#include <stdio.h>
#include <string.h>

/* Diagnostics read the same original images and VM table as the loader/procfs.
   No second module registry or guest memory changes are needed. */
typedef struct {
    uint32_t name;
#ifdef _WIN64
    unsigned char info, other;
    uint16_t section;
    uint64_t value, size;
#else
    uint32_t value, size;
    unsigned char info, other;
    uint16_t section;
#endif
} NrSymbol;

static int bounded(const NrElf *elf, uint64_t at, uint64_t bytes) {
    return at <= elf->file_size && bytes <= elf->file_size - at;
}

static const char *symbol_at(const NrElf *elf, uintptr_t pc, uintptr_t *displacement) {
    const NrElfHeader *h = elf->header;
    if (!h || pc < elf->bias || h->shentsize != sizeof(NrElfSection) ||
        !bounded(elf, h->shoff, (uint64_t)h->shnum * sizeof(NrElfSection))) return NULL;
    const NrElfSection *sections = (const NrElfSection *)(elf->file + h->shoff);
    const char *best = NULL;
    uintptr_t distance = UINTPTR_MAX, relative = pc - elf->bias;
    for (unsigned i = 0; i < h->shnum; ++i) {
        const NrElfSection *s = &sections[i];
        if ((s->type != 2 && s->type != 11) || s->entsize != sizeof(NrSymbol) ||
            s->link >= h->shnum || !bounded(elf, s->offset, s->size)) continue;
        const NrElfSection *strings = &sections[s->link];
        if (!bounded(elf, strings->offset, strings->size)) continue;
        for (uint64_t at = 0; at + sizeof(NrSymbol) <= s->size; at += sizeof(NrSymbol)) {
            NrSymbol symbol;
            memcpy(&symbol, elf->file + s->offset + at, sizeof(symbol));
            if ((symbol.info & 15) != 2 || !symbol.section || !symbol.size ||
                relative < symbol.value || relative - symbol.value >= symbol.size ||
                relative - symbol.value >= distance || symbol.name >= strings->size) continue;
            const char *name = (const char *)elf->file + strings->offset + symbol.name;
            if (!memchr(name, 0, (size_t)(strings->size - symbol.name))) continue;
            best = name; distance = relative - (uintptr_t)symbol.value;
        }
    }
    if (best) *displacement = distance;
    return best;
}

static const char *interpreter_path(const NrRun *run) {
    for (unsigned i = 0; i < run->program.header->phnum; ++i) {
        const NrElfProgram *p = &run->program.programs[i];
        if (p->type == 3) return (const char *)run->program.file + p->offset;
    }
    return "[interpreter]";
}

static int contains(const NrElf *elf, uintptr_t pc) {
    return elf->allocation && pc >= (uintptr_t)elf->allocation &&
           pc - (uintptr_t)elf->allocation < elf->allocation_size;
}

static void location(NrRun *run, uintptr_t pc) {
    const NrElf *elf = NULL;
    const char *path = "[unresolved]";
    if (contains(&run->program, pc)) { elf = &run->program; path = run->argc ? run->argv[0] : "[program]"; }
    else if (contains(&run->interpreter, pc)) { elf = &run->interpreter; path = interpreter_path(run); }
    else for (unsigned i = 0; i < run->vm_count; ++i) {
        if (contains(&run->vm[i].image, pc)) {
            elf = &run->vm[i].image;
            path = run->vm[i].path ? run->vm[i].path : "[dependency]";
            break;
        }
    }
    fprintf(stderr, " module=%s", path);
    if (elf) {
        fprintf(stderr, " sha256=%s elf_pc=0x%llx", elf->sha256, (unsigned long long)(pc - elf->bias));
        uintptr_t offset;
        const char *symbol = symbol_at(elf, pc, &offset);
        if (symbol) fprintf(stderr, " symbol=%s+0x%llx", symbol, (unsigned long long)offset);
        else fputs(" symbol=[unavailable]", stderr);
    }
}

void nr_log_image(const NrElf *elf, const char *path) {
    _lock_file(stderr);
    fprintf(stderr, "ELF image path=%s bits=%u type=%u sha256=%s entry=0x%llx base=0x%llx bytes=%zu\n",
            path, (unsigned)(sizeof(void *) * 8), elf->header->type, elf->sha256,
            (unsigned long long)elf->entry, (unsigned long long)(uintptr_t)elf->allocation, elf->allocation_size);
    for (unsigned i = 0; i < elf->header->phnum; ++i) {
        const NrElfProgram *p = &elf->programs[i];
        if (p->type == 7) fprintf(stderr, "ELF TLS path=%s address=0x%llx filesz=%llu memsz=%llu align=%llu\n",
                                 path, (unsigned long long)(elf->bias + p->vaddr), (unsigned long long)p->filesz,
                                 (unsigned long long)p->memsz, (unsigned long long)p->align);
    }
    _unlock_file(stderr);
}

void nr_log_gap(NrRun *run, uintptr_t number, const uintptr_t args[6], uintptr_t pc) {
    NrThread *thread = nr_current();
    unsigned count = 1;
    if (thread) {
        for (unsigned i = 0; i < 32; ++i) {
            NrGap *gap = &thread->gaps[i];
            if (!gap->count || (gap->number == number && gap->pc == pc)) {
                gap->number = number; gap->pc = pc; count = ++gap->count; break;
            }
        }
    }
    /* First and powers of two retain repeat evidence without flooding logs. */
    if (count & (count - 1)) return;
    _lock_file(stderr);
    fprintf(stderr, "syscall gap service=%s bits=%u tid=%lu nr=%llu pc=0x%llx count=%u args=",
            run->argc ? run->argv[0] : "[program]", (unsigned)(sizeof(void *) * 8), GetCurrentThreadId(),
            (unsigned long long)number, (unsigned long long)pc, count);
    for (unsigned i = 0; i < 6; ++i) fprintf(stderr, "%s0x%llx", i ? "," : "", (unsigned long long)args[i]);
    if (TryEnterCriticalSection(&run->memory_lock)) { location(run, pc); LeaveCriticalSection(&run->memory_lock); }
    else fputs(" module=[table busy]", stderr);
    fputc('\n', stderr);
    _unlock_file(stderr);
}

static int read_bytes(uintptr_t address, void *output, size_t bytes) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, output, bytes, &read) && read == bytes;
}

static void thread_record(const char *const labels[3], const uintptr_t values[3], const char *reason) {
    /* Exit runs on a guest stack. Avoid CRT per-thread teardown/stdio locks
       racing exit_group on another thread; emit one bounded kernel write. */
    char line[256], *out = line;
    for (unsigned i = 0; i < 3; ++i) {
        for (const char *p = labels[i]; *p; ++p) *out++ = *p;
        int started = 0;
        for (unsigned n = sizeof(uintptr_t) * 2; n; --n) {
            unsigned digit = (unsigned)((values[i] >> ((n - 1) * 4)) & 15);
            if (digit || started || n == 1) { *out++ = "0123456789abcdef"[digit]; started = 1; }
        }
    }
    if (reason) {
        *out++ = ' ';
        for (unsigned i = 0; reason[i] && i < 48; ++i) *out++ = reason[i];
    }
    *out++ = '\r'; *out++ = '\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)(out - line), &written, NULL);
}

void nr_log_thread_exit(const NrThread *thread, uintptr_t status) {
    const char *labels[] = {"guest thread exit tid=0x", " tls=0x", " status=0x"};
    uintptr_t values[] = {thread->tid, thread->tls_base, status};
    thread_record(labels, values, NULL);
}

void nr_log_robust_failure(const NrThread *thread, uintptr_t address, const char *reason) {
    const char *labels[] = {"robust cleanup stopped tid=0x", " head=0x", " address=0x"};
    uintptr_t values[] = {thread->tid, thread->robust_head, address};
    thread_record(labels, values, reason);
}

static void dump(const char *label, uintptr_t address, size_t bytes) {
    unsigned char data[64];
    if (bytes > sizeof(data)) bytes = sizeof(data);
    fprintf(stderr, "%s address=0x%llx bytes=", label, (unsigned long long)address);
    if (!read_bytes(address, data, bytes)) fputs("[unreadable]", stderr);
    else for (size_t i = 0; i < bytes; ++i) fprintf(stderr, "%02x", data[i]);
    fputc('\n', stderr);
}

void nr_log_crash(NrRun *run, EXCEPTION_POINTERS *exception) {
    _lock_file(stderr);
    const CONTEXT *c = exception->ContextRecord;
#ifdef _WIN64
    uintptr_t pc = c->Rip, sp = c->Rsp, bp = c->Rbp;
    const uint64_t regs[] = {c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15};
    const char *names[] = {"rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
#else
    uintptr_t pc = c->Eip, sp = c->Esp, bp = c->Ebp;
    const uint64_t regs[] = {c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi};
    const char *names[] = {"eax","ebx","ecx","edx","esi","edi"};
#endif
    fprintf(stderr, "unhandled exception code=0x%08lx pc=0x%llx service=%s bits=%u tid=%lu\n",
            exception->ExceptionRecord->ExceptionCode, (unsigned long long)pc,
            run->argc ? run->argv[0] : "[program]", (unsigned)(sizeof(void *) * 8), GetCurrentThreadId());
    fprintf(stderr, "registers pc=0x%llx sp=0x%llx bp=0x%llx flags=0x%lx tls=0x%llx",
            (unsigned long long)pc, (unsigned long long)sp, (unsigned long long)bp, c->EFlags,
            (unsigned long long)(nr_current() ? nr_current()->tls_base : 0));
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
        fprintf(stderr, " %s=0x%llx", names[i], (unsigned long long)regs[i]);
    fputc('\n', stderr);
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        fprintf(stderr, "fault access=%llu address=0x%llx\n", (unsigned long long)record->ExceptionInformation[0],
                (unsigned long long)record->ExceptionInformation[1]);
    dump("instructions", pc, 16); dump("stack", sp, 64); dump("frame", bp, 64);
    if (!TryEnterCriticalSection(&run->memory_lock)) {
        fputs("frames module=[table busy]\n", stderr); _unlock_file(stderr); return;
    }
    fputs("frame[0]", stderr); location(run, pc); fputc('\n', stderr);
    /* Bounded frame-pointer walk. Missing frame pointers remain explicit. */
    for (unsigned i = 1; i <= 16; ++i) {
        uintptr_t frame[2];
        if ((bp & (sizeof(uintptr_t) - 1)) || !read_bytes(bp, frame, sizeof(frame)) || !frame[1]) break;
        fprintf(stderr, "frame[%u] return=0x%llx", i, (unsigned long long)frame[1]);
        location(run, frame[1] - 1); fputc('\n', stderr);
        if (frame[0] <= bp || frame[0] - bp > 1024 * 1024) break;
        bp = frame[0];
    }
    LeaveCriticalSection(&run->memory_lock);
    _unlock_file(stderr);
}

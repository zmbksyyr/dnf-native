#include "run.h"
#include <bcrypt.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define NR_STACK_BYTES (512 * 1024)

static NrRun *active;
__attribute__((noreturn)) void nr_enter(uintptr_t entry, uintptr_t stack);

static int in_image(const NrElf *elf, uintptr_t pc) {
    if (!elf->allocation) return 0;
    for (unsigned i = 0; i < elf->header->phnum; ++i) {
        NrElfProgram *p = &elf->programs[i];
        if (p->type == 1 && (p->flags & 1) && pc >= elf->bias + p->vaddr &&
            pc - (elf->bias + p->vaddr) < p->memsz)
            return 1;
    }
    return 0;
}

static int executable(uintptr_t pc) {
    if (in_image(&active->program, pc) || in_image(&active->interpreter, pc)) return 1;
    for (unsigned i = 0; i < 64; ++i) if (in_image(&active->vm[i].image, pc)) return 1;
    return 0;
}

int nr_memory(uintptr_t address, uintptr_t bytes, int write) {
    if (!bytes) return 1;
    if (address > UINTPTR_MAX - bytes) return 0;
    uintptr_t end = address + bytes;
    while (address < end) {
        uintptr_t covered = address;
        if (address >= active->stack_low && address < active->stack_high)
            covered = active->stack_high;
        if (address >= active->heap_base && address - active->heap_base < active->heap_committed)
            covered = active->heap_base + active->heap_committed;
        const NrElf *images[] = {&active->program, &active->interpreter};
        for (unsigned j = 0; j < 2; ++j) {
            const NrElf *elf = images[j];
            if (!elf->allocation) continue;
            for (unsigned i = 0; i < elf->header->phnum; ++i) {
                NrElfProgram *p = &elf->programs[i];
                uintptr_t first = elf->bias + (uintptr_t)p->vaddr;
                if (p->type == 1 && (p->flags & (write ? 2 : 5)) && address >= first &&
                    address - first < p->memsz && first + p->memsz > covered)
                    covered = first + (uintptr_t)p->memsz;
            }
        }
        for (unsigned i = 0; i < 64; ++i) {
            NrVm *v = &active->vm[i];
            uintptr_t first = (uintptr_t)v->base;
            if (first && address >= first && address - first < v->size && first + v->size > covered)
                covered = first + v->size;
        }
        if (covered == address) return 0;
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        if (write && !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) return 0;
        uintptr_t region_end = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (covered > region_end) covered = region_end;
        address = covered;
    }
    return 1;
}

static HANDLE output_handle(uintptr_t fd) {
    if (fd >= 256 || !(active->file_access[fd] & 2)) return INVALID_HANDLE_VALUE;
    HANDLE file = active->files[fd];
    if (!file || file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SetLastError(NO_ERROR);
    if (GetFileType(file) == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) return INVALID_HANDLE_VALUE;
    return file;
}

static intptr_t write_copy(HANDLE file, const void *bytes, DWORD count) {
    DWORD written = 0;
    if (WriteFile(file, bytes, count, &written, NULL)) return (intptr_t)written;
    DWORD error = GetLastError();
    return error == ERROR_INVALID_HANDLE ? -9 : error == ERROR_BROKEN_PIPE ? -32 : -5;
}

static intptr_t dispatch(uintptr_t number, const uintptr_t args[6], uintptr_t pc) {
    uintptr_t a = args[0], b = args[1], c = args[2];
    intptr_t result;
    if (nr_net(active, number, args, &result)) return result;
    if (nr_files(active, number, args, &result)) return result;
    if (nr_vm(active, number, args, &result)) return result;
    if (nr_signals(active, number, args, &result)) return result;
#ifdef _WIN64
    enum {SYS_TIME = 201, SYS_GETTIMEOFDAY = 96, SYS_GETRLIMIT = 97, SYS_TIMES = 100, SYS_NANOSLEEP = 35};
#else
    enum {SYS_TIME = 13, SYS_GETTIMEOFDAY = 78, SYS_GETRLIMIT = 191, SYS_TIMES = 43, SYS_NANOSLEEP = 162};
#endif
    if (number == SYS_TIMES) {
        if (a) {
            if (!nr_memory(a, 4 * sizeof(uintptr_t), 1)) return -14;
            FILETIME created, ended, kernel, user;
            if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return -5;
            uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
            uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
            uintptr_t values[] = {(uintptr_t)(u / 100000), (uintptr_t)(k / 100000), 0, 0};
            memcpy((void *)a, values, sizeof(values));
        }
        return (intptr_t)(GetTickCount64() / 10);
    }
    if (number == SYS_NANOSLEEP) {
        if (!nr_memory(a, 2 * sizeof(uintptr_t), 0)) return -14;
        intptr_t input[2]; memcpy(input, (const void *)a, sizeof(input));
        if (input[0] < 0 || input[1] < 0 || input[1] >= 1000000000) return -22;
        if (b && !nr_memory(b, 2 * sizeof(uintptr_t), 1)) return -14;
        uint64_t milliseconds = (uint64_t)input[0] * 1000 + ((uint64_t)input[1] + 999999) / 1000000;
        while (milliseconds) {
            DWORD duration = milliseconds > 60000 ? 60000 : (DWORD)milliseconds;
            Sleep(duration); milliseconds -= duration;
        }
        return 0;
    }
    if (number == SYS_TIME || number == SYS_GETTIMEOFDAY) {
        if (number == SYS_GETTIMEOFDAY && b) return -38;
        FILETIME time;
        GetSystemTimeAsFileTime(&time);
        uint64_t ticks = (((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime) - 116444736000000000ULL;
        uintptr_t seconds = (uintptr_t)(ticks / 10000000);
        if (a) {
            size_t bytes = sizeof(uintptr_t) * (number == SYS_TIME ? 1 : 2);
            if (!nr_memory(a, bytes, 1)) return -14;
            uintptr_t values[] = {seconds, (uintptr_t)(ticks % 10000000 / 10)};
            memcpy((void *)a, values, bytes);
        }
        return number == SYS_TIME ? (intptr_t)seconds : 0;
    }
    if (number == SYS_GETRLIMIT) {
        uintptr_t limit;
        if (a == 3) limit = NR_STACK_BYTES;
        else if (a == 7) limit = 256;
        else return -38;
        if (!nr_memory(b, 2 * sizeof(uintptr_t), 1)) return -14;
        uintptr_t values[] = {limit, limit};
        memcpy((void *)b, values, sizeof(values));
        return 0;
    }
#ifdef _WIN64
    if (number == 158) {
        if (a == 0x1002) {
            if (b >= 0x800000000000ULL) return -1;
            nr_current()->tls_base = b;
            return 0;
        }
        if (a == 0x1003) {
            if (!nr_memory(b, sizeof(uintptr_t), 1)) return -14;
            memcpy((void *)b, &nr_current()->tls_base, sizeof(uintptr_t));
            return 0;
        }
        return -38;
    }
#else
    if (number == 243) {
        if (!nr_memory(a, 16, 1)) return -14;
        uint32_t descriptor[4];
        memcpy(descriptor, (const void *)a, sizeof(descriptor));
        if ((descriptor[0] != UINT32_MAX && descriptor[0] != 12) || descriptor[2] != 0xfffff || descriptor[3] != 0x51) return -38;
        descriptor[0] = 12;
        nr_current()->tls_base = descriptor[1];
        memcpy((void *)a, descriptor, sizeof(descriptor));
        return 0;
    }
#endif
#ifdef _WIN64
    enum {SYS_WRITE = 1, SYS_WRITEV = 20, SYS_BRK = 12, SYS_EXIT = 60, SYS_EXIT_GROUP = 231, SYS_GETPID = 39, SYS_GETTID = 186, SYS_UNAME = 63};
#else
    enum {SYS_WRITE = 4, SYS_WRITEV = 146, SYS_BRK = 45, SYS_EXIT = 1, SYS_EXIT_GROUP = 252, SYS_GETPID = 20, SYS_GETTID = 224, SYS_UNAME = 122};
#endif
    if (number == SYS_UNAME) {
        if (!nr_memory(a, 390, 1)) return -14;
        char values[6][65] = {"Linux", "dnf-native", "2.6.32", "dnf-native Linux ABI prototype", "", "(none)"};
        strcpy(values[4], sizeof(void *) == 8 ? "x86_64" : "i686");
        memcpy((void *)a, values, sizeof(values));
        return 0;
    }
    if (number == SYS_EXIT || number == SYS_EXIT_GROUP) {
        fprintf(stderr, "guest exit status=%u\n", (unsigned)a & 255);
        fflush(NULL);
        ExitProcess((UINT)a & 255);
    }
    if (number == SYS_GETPID) return (intptr_t)GetCurrentProcessId();
    if (number == SYS_GETTID) return (intptr_t)GetCurrentThreadId();
    if (number == SYS_BRK) {
        if (!a || a < active->heap_base || a - active->heap_base > 64 * 1024 * 1024)
            return (intptr_t)active->heap_break;
        size_t committed = (size_t)((a - active->heap_base + 4095) & ~(uintptr_t)4095);
        if (committed > active->heap_committed &&
            !VirtualAlloc((void *)(active->heap_base + active->heap_committed),
                          committed - active->heap_committed, MEM_COMMIT, PAGE_READWRITE))
            return (intptr_t)active->heap_break;
        if (committed < active->heap_committed &&
            !VirtualFree((void *)(active->heap_base + committed),
                         active->heap_committed - committed, MEM_DECOMMIT))
            return (intptr_t)active->heap_break;
        active->heap_committed = committed;
        active->heap_break = a;
        return (intptr_t)a;
    }
    if (number == SYS_WRITE) {
        HANDLE file = output_handle(a);
        if (file == INVALID_HANDLE_VALUE) return -9;
        if (c > 0x7ffff000) c = 0x7ffff000;
        if (!nr_memory(b, c, 0)) return -14;
        if (!c) return 0;
        unsigned char *copy = HeapAlloc(GetProcessHeap(), 0, (size_t)c);
        if (!copy) return -12;
        memcpy(copy, (const void *)b, (size_t)c);
        intptr_t result = write_copy(file, copy, (DWORD)c);
        HeapFree(GetProcessHeap(), 0, copy);
        return result;
    }
    if (number == SYS_WRITEV) {
        HANDLE file = output_handle(a);
        if (file == INVALID_HANDLE_VALUE) return -9;
        if (c > 1024) return -22;
        if (!c) return 0;
        if (!nr_memory(b, c * 2 * sizeof(uintptr_t), 0)) return -14;
        uintptr_t vectors[2048];
        memcpy(vectors, (const void *)b, (size_t)c * 2 * sizeof(uintptr_t));
        size_t total = 0;
        for (size_t i = 0; i < c; ++i) {
            uintptr_t length = vectors[2 * i + 1];
            if (length > INTPTR_MAX - total) return -22;
            if (!nr_memory(vectors[2 * i], length, 0)) return -14;
            total += (size_t)length;
        }
        if (total > 0x7ffff000) total = 0x7ffff000;
        if (!total) return 0;
        unsigned char *copy = HeapAlloc(GetProcessHeap(), 0, total);
        if (!copy) return -12;
        size_t cursor = 0;
        for (size_t i = 0; i < c; ++i) {
            size_t length = (size_t)vectors[2 * i + 1];
            if (length > total - cursor) length = total - cursor;
            if (length) memcpy(copy + cursor, (const void *)vectors[2 * i], length);
            cursor += length;
            if (cursor == total) break;
        }
        intptr_t result = write_copy(file, copy, (DWORD)total);
        HeapFree(GetProcessHeap(), 0, copy);
        return result;
    }
    fprintf(stderr, "syscall gap nr=%llu pc=0x%llx args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx\n",
            (unsigned long long)number, (unsigned long long)pc,
            (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
            (unsigned long long)args[3], (unsigned long long)args[4], (unsigned long long)args[5]);
    return -38;
}

static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *exception) {
    if (!nr_current()) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = exception->ContextRecord;
    if (exception->ExceptionRecord->ExceptionCode == 0xe0444e46u) {
        *c = nr_current()->initial_context;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#ifdef _WIN64
    uintptr_t pc = c->Rip;
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        (pc == 0xffffffffff600000ULL || pc == 0xffffffffff600400ULL) && nr_memory(c->Rsp, 8, 0)) {
        uintptr_t args[] = {c->Rdi, c->Rsi, c->Rdx, c->R10, c->R8, c->R9};
        c->Rax = (DWORD64)dispatch(pc == 0xffffffffff600000ULL ? 96 : 201, args, pc);
        memcpy(&c->Rip, (const void *)c->Rsp, 8);
        c->Rsp += 8;
        if (!nr_deliver_signal(active, c)) ExitProcess(124);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#else
    uintptr_t pc = c->Eip;
#endif
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && nr_current()->in_signal &&
        pc == (uintptr_t)active->signal_code) {
        *c = nr_current()->saved_context;
        nr_current()->signal_mask = nr_current()->saved_mask;
        nr_current()->in_signal = 0;
        if (!nr_deliver_signal(active, c)) ExitProcess(124);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && nr_tls_finish(active, c))
        return EXCEPTION_CONTINUE_EXECUTION;
    int tls = 0;
    int site = nr_syscall_site(&active->program, pc, &tls) || nr_syscall_site(&active->interpreter, pc, &tls);
    for (unsigned i = 0; !site && i < 64; ++i) site = nr_syscall_site(&active->vm[i].image, pc, &tls);
    if (site && executable(pc) &&
        exception->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        if (tls) {
            const NrTrap *record = NULL;
            const NrElf *images[66] = {&active->program, &active->interpreter};
            for (unsigned i = 0; i < 64; ++i) images[i + 2] = &active->vm[i].image;
            for (unsigned i = 0; !record && i < 66; ++i)
                for (size_t j = 0; j < images[i]->trap_count; ++j)
                    if (images[i]->traps[j].pc == pc) { record = &images[i]->traps[j]; break; }
            if (nr_tls(active, c, record)) return EXCEPTION_CONTINUE_EXECUTION;
            fprintf(stderr, "unimplemented guest TLS site pc=0x%llx\n", (unsigned long long)pc);
            fflush(NULL);
            ExitProcess(126);
        }
#ifdef _WIN64
        if (c->Rax == 15 && nr_current()->in_signal) {
            *c = nr_current()->saved_context; nr_current()->signal_mask = nr_current()->saved_mask; nr_current()->in_signal = 0;
            if (!nr_deliver_signal(active, c)) ExitProcess(124);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        uintptr_t args[] = {c->Rdi, c->Rsi, c->Rdx, c->R10, c->R8, c->R9};
        intptr_t result;
        c->Rax = (DWORD64)(nr_threads(active, c->Rax, args, c, &result) ? result : dispatch(c->Rax, args, pc));
        c->Rcx = pc + 2;
        c->R11 = c->EFlags & ~0x10000u;
        c->Rip = pc + 2;
        if (!nr_deliver_signal(active, c)) ExitProcess(124);
        return EXCEPTION_CONTINUE_EXECUTION;
#endif
    }
#ifndef _WIN64
    DWORD code = exception->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ILLEGAL_INSTRUCTION) &&
        executable(pc) &&
        nr_memory(pc, 2, 0) && !memcmp((const void *)pc, "\xcd\x80", 2)) {
        if (c->Eax == 173 && nr_current()->in_signal) {
            *c = nr_current()->saved_context; nr_current()->signal_mask = nr_current()->saved_mask; nr_current()->in_signal = 0;
            if (!nr_deliver_signal(active, c)) ExitProcess(124);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        uintptr_t args[] = {c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
        intptr_t result;
        c->Eax = (DWORD)(nr_threads(active, c->Eax, args, c, &result) ? result : dispatch(c->Eax, args, pc));
        c->Eip = pc + 2;
        if (!nr_deliver_signal(active, c)) ExitProcess(124);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *exception) {
#ifdef _WIN64
    uintptr_t pc = exception->ContextRecord->Rip;
#else
    uintptr_t pc = exception->ContextRecord->Eip;
#endif
    fprintf(stderr, "unhandled exception code=0x%08lx pc=0x%llx\n",
            exception->ExceptionRecord->ExceptionCode, (unsigned long long)pc);
    fflush(NULL);
    ExitProcess(125);
    return EXCEPTION_EXECUTE_HANDLER;
}

static uintptr_t push_string(uintptr_t *cursor, const char *text) {
    size_t bytes = strlen(text) + 1;
    *cursor -= bytes;
    memcpy((void *)*cursor, text, bytes);
    return *cursor;
}

static DWORD WINAPI start_guest(void *argument) {
    NrRun *run = argument;
    nr_thread_attach(&run->threads[0]);
    unsigned char *memory = _alloca(NR_STACK_BYTES);
    run->stack_low = (uintptr_t)memory;
    run->stack_high = (uintptr_t)memory + NR_STACK_BYTES;
    uintptr_t cursor = run->stack_high - 256;
    uintptr_t arguments[256];
    for (int i = run->argc - 1; i >= 0; --i) arguments[i] = push_string(&cursor, run->argv[i]);
    uintptr_t env_path = push_string(&cursor, "PATH=/usr/bin:/bin");
    uintptr_t env_lang = push_string(&cursor, "LANG=C");
    uintptr_t platform = push_string(&cursor, sizeof(void *) == 8 ? "x86_64" : "i686");
    cursor -= 16;
    uintptr_t random = cursor;
    if (BCryptGenRandom(NULL, (PUCHAR)random, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        ExitProcess(124);
    uintptr_t phdr = 0;
    NrElf *program = &run->program;
    for (unsigned i = 0; i < program->header->phnum; ++i) {
        NrElfProgram *p = &program->programs[i];
        size_t bytes = program->header->phnum * sizeof(NrElfProgram);
        if (p->type == 1 && program->header->phoff >= p->offset &&
            program->header->phoff - p->offset <= p->filesz &&
            bytes <= p->filesz - (program->header->phoff - p->offset))
            phdr = program->bias + (uintptr_t)p->vaddr + (uintptr_t)(program->header->phoff - p->offset);
    }
    if (!phdr) { fputs("program headers not mapped\n", stderr); ExitProcess(124); }
    uintptr_t aux[][2] = {
        {3, phdr}, {4, sizeof(NrElfProgram)}, {5, program->header->phnum}, {6, 4096},
        {7, run->interpreter.allocation ? run->interpreter.bias : 0}, {8, 0}, {9, program->entry},
        {11, 1000}, {12, 1000}, {13, 1000}, {14, 1000}, {15, platform},
        {16, 0}, {17, 100}, {23, 0}, {25, random}, {26, 0}, {31, arguments[0]}, {0, 0},
    };
    size_t words = 1 + (size_t)run->argc + 1 + 2 + 1 + sizeof(aux) / sizeof(uintptr_t);
    uintptr_t stack = (cursor - words * sizeof(uintptr_t)) & ~(uintptr_t)15;
    if (stack < run->stack_low) ExitProcess(124);
    uintptr_t *out = (uintptr_t *)stack;
    *out++ = (uintptr_t)run->argc;
    for (int i = 0; i < run->argc; ++i) *out++ = arguments[i];
    *out++ = 0;
    *out++ = env_path;
    *out++ = env_lang;
    *out++ = 0;
    memcpy(out, aux, sizeof(aux));
    fprintf(stderr, "enter guest bits=%zu entry=0x%llx stack=0x%llx argc=%d\n",
            sizeof(void *) * 8, (unsigned long long)(run->interpreter.allocation ? run->interpreter.entry : program->entry),
            (unsigned long long)stack, run->argc);
    nr_enter(run->interpreter.allocation ? run->interpreter.entry : program->entry, stack);
}

int nr_run(NrRun *run) {
    if (run->argc < 1 || run->argc > 256) return 2;
    size_t bytes = 0;
    for (int i = 0; i < run->argc; ++i) {
        size_t n = strlen(run->argv[i]) + 1;
        if (n > 256 * 1024 || bytes > 256 * 1024 - n) return 2;
        bytes += n;
    }
    active = run;
    run->thread_count = 1;
    if (!nr_thread_init(run)) return 1;
    run->signal_code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!run->signal_code) return 1;
    memcpy(run->signal_code, "\x0f\x0b", 2);
    DWORD old;
    if (!VirtualProtect(run->signal_code, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), run->signal_code, 2)) return 1;
    DWORD standard[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (unsigned i = 0; i < 3; ++i) {
        HANDLE handle = GetStdHandle(standard[i]);
        if (handle && handle != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &run->files[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
            run->file_access[i] = i == 0 ? 1 : 2;
    }
    run->heap_base = (uintptr_t)VirtualAlloc(NULL, 64 * 1024 * 1024, MEM_RESERVE, PAGE_READWRITE);
    if (!run->heap_base) return 1;
    run->heap_break = run->heap_base;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(unhandled);
    if (!AddVectoredExceptionHandler(1, trap_handler)) return 1;
    HANDLE thread = CreateThread(NULL, 16 * 1024 * 1024, start_guest, run, 0, NULL);
    if (!thread) return 1;
    WaitForSingleObject(thread, INFINITE);
    for (unsigned i = 1; i < 64; ++i)
        if (run->threads[i].handle) WaitForSingleObject(run->threads[i].handle, INFINITE);
    CloseHandle(thread);
    fputs("guest thread returned without process exit\n", stderr);
    return 1;
}

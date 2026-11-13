#include "run.h"
#include <stdio.h>
#include <string.h>

static DWORD protect(uintptr_t flags) {
    if (flags & 4) return flags & 2 ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (flags & 2) return PAGE_READWRITE;
    return flags & 1 ? PAGE_READONLY : PAGE_NOACCESS;
}

static int contains(uintptr_t base, size_t size, uintptr_t address, size_t bytes) {
    return base && address >= base && address - base <= size && bytes <= size - (address - base);
}

static intptr_t mapping(NrRun *run, const uintptr_t args[6]) {
    uintptr_t address = args[0], flags = args[3], fd = args[4];
    size_t length = (size_t)args[1];
    uint64_t offset = args[5];
#ifndef _WIN64
    offset *= 4096;
#endif
    if (!length || length > SIZE_MAX - 4095 || (args[2] & ~7u) || (offset & 4095)) return -22;
    length = (length + 4095) & ~(size_t)4095;
    if (flags & ~(uintptr_t)(2 | 0x10 | 0x20 | 0x800 | 0x20000)) {
        fprintf(stderr, "mmap unsupported flags=0x%llx\n", (unsigned long long)flags); return -38;
    }
    if (!(flags & 2)) return -38;
    if (flags & 0x10) {
        if (address & 4095) return -22;
        NrVm *v = NULL;
        for (unsigned i = 0; i < 64; ++i)
            if (contains((uintptr_t)run->vm[i].base, run->vm[i].size, address, length)) { v = &run->vm[i]; break; }
        if (!v) return -38;
        if (!(flags & 0x20) && (!v->image.file || fd >= 256 || !run->files[fd] || !v->source)) return -38;
        if (!(flags & 0x20)) {
            BY_HANDLE_FILE_INFORMATION a, b;
            if (!GetFileInformationByHandle(v->source, &a) || !GetFileInformationByHandle(run->files[fd], &b) ||
                a.dwVolumeSerialNumber != b.dwVolumeSerialNumber || a.nFileIndexHigh != b.nFileIndexHigh || a.nFileIndexLow != b.nFileIndexLow)
                return -38;
            if (offset >= v->image.file_size) return -38;
        }
        if (!VirtualAlloc((void *)address, length, MEM_COMMIT, PAGE_READWRITE)) return -12;
        DWORD old;
        if (!VirtualProtect((void *)address, length, PAGE_READWRITE, &old)) return -13;
        memset((void *)address, 0, length);
        if (!(flags & 0x20)) {
            size_t bytes = v->image.file_size - (size_t)offset;
            if (bytes > length) bytes = length;
            memcpy((void *)address, v->image.file + offset, bytes);
        }
        if (!VirtualProtect((void *)address, length, protect(args[2]), &old)) return -13;
        return (intptr_t)address;
    }
    NrVm *v = NULL;
    for (unsigned i = 0; i < 64; ++i) if (!run->vm[i].base) { v = &run->vm[i]; break; }
    if (!v) return -12;
    if (flags & 0x20) {
        if (args[2] & 4) return -38;
        v->base = VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, protect(args[2]));
        if (!v->base) return -12;
        v->size = length;
        return (intptr_t)v->base;
    }
    /* Deliberately restricted to immutable ELF dependency snapshots. This
       does not implement general file mmap, shared mappings or lazy COW. */
    if (offset || fd >= 256 || !run->files[fd]) return -38;
    wchar_t path[32768];
    DWORD n = GetFinalPathNameByHandleW(run->files[fd], path, 32768, FILE_NAME_NORMALIZED);
    if (!n || n >= 32768) return -5;
    const char *error;
    if (!nr_elf_open(&v->image, path, NULL, &error)) return -38;
    if (v->image.header->type != 3 || v->image.low || length > v->image.allocation_size ||
        !nr_elf_map(&v->image, &error) || !nr_adapt(&v->image, &error)) {
        fprintf(stderr, "ELF dependency snapshot failed: %s\n", error ? error : "unsupported extent");
        nr_elf_close(&v->image); return -38;
    }
    if (!DuplicateHandle(GetCurrentProcess(), run->files[fd], GetCurrentProcess(), &v->source, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        nr_elf_close(&v->image); return -5;
    }
    v->base = v->image.allocation;
    v->size = v->image.allocation_size;
    fprintf(stderr, "ELF dependency private snapshot sha256=%s base=0x%llx bytes=%zu\n", v->image.sha256,
            (unsigned long long)(uintptr_t)v->base, v->size);
    return (intptr_t)v->base;
}

int nr_vm(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
#ifdef _WIN64
    enum {MMAP = 9, MPROTECT = 10, MUNMAP = 11, MADVISE = 28};
#else
    enum {MMAP = 192, MPROTECT = 125, MUNMAP = 91, MADVISE = 219};
#endif
    if (number == MMAP) { *result = mapping(run, args); return 1; }
    if (number != MPROTECT && number != MUNMAP && number != MADVISE) return 0;
    uintptr_t address = args[0];
    size_t length = args[1];
    if ((address & 4095) || !length || length > SIZE_MAX - 4095) { *result = -22; return 1; }
    length = (length + 4095) & ~(size_t)4095;
    NrVm *v = NULL;
    for (unsigned i = 0; i < 64; ++i)
        if (contains((uintptr_t)run->vm[i].base, run->vm[i].size, address, length)) { v = &run->vm[i]; break; }
    if (number == MADVISE) {
        if (!v) *result = -12;
        else if (args[2] != 4 || v->image.file) *result = -38;
        else if (!nr_memory(address, length, 1)) *result = -38;
        else if (!VirtualFree((void *)address, length, MEM_DECOMMIT)) *result = -12;
        else *result = VirtualAlloc((void *)address, length, MEM_COMMIT, PAGE_READWRITE) ? 0 : -12;
        return 1;
    }
    if (number == MUNMAP) {
        if (!v) { *result = -38; return 1; }
        if (address == (uintptr_t)v->base && length == v->size) {
            if (v->image.file) nr_elf_close(&v->image); else VirtualFree(v->base, 0, MEM_RELEASE);
            if (v->source) CloseHandle(v->source);
            memset(v, 0, sizeof(*v));
            *result = 0;
        } else {
            *result = VirtualFree((void *)address, length, MEM_DECOMMIT) ? 0 : -22;
        }
        return 1;
    }
    int owned = v != NULL;
    const NrElf *images[] = {&run->program, &run->interpreter};
    for (unsigned i = 0; i < 2; ++i)
        if (contains((uintptr_t)images[i]->allocation, images[i]->allocation_size, address, length)) owned = 1;
    if (!owned) { *result = -12; return 1; }
    if ((args[2] & ~7u) || ((args[2] & 4) && v && !v->image.file)) { *result = -38; return 1; }
    DWORD old;
    *result = VirtualProtect((void *)address, length, protect(args[2]), &old) ? 0 : -12;
    return 1;
}

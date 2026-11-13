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

static char *guest_path_from_handle(NrRun *run, HANDLE file) {
    wchar_t path[32768];
    DWORD count = GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED);
    if (!count || count >= 32768) return NULL;
    const wchar_t *host = path;
    if (!wcsncmp(host, L"\\\\?\\", 4)) host += 4;
    size_t root_length = wcslen(run->root);
    if (_wcsnicmp(host, run->root, root_length) ||
        (host[root_length] && host[root_length] != L'\\')) return NULL;
    const wchar_t *relative = host + root_length;
    while (*relative == L'\\') ++relative;
    size_t length = wcslen(relative);
    char *guest = HeapAlloc(GetProcessHeap(), 0, length + 2);
    if (!guest) return NULL;
    guest[0] = '/';
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative, (int)length,
                             guest + 1, (int)length + 1, NULL, NULL)) {
        HeapFree(GetProcessHeap(), 0, guest);
        return NULL;
    }
    guest[length + 1] = 0;
    for (char *p = guest; *p; ++p) if (*p == '\\') *p = '/';
    return guest;
}

static int mapped(const NrVm *v, uintptr_t address, size_t length) {
    if (!v->mapped_pages) return 1;
    size_t first = (address - (uintptr_t)v->base) / 4096;
    for (size_t i = first; i < first + length / 4096; ++i)
        if (!(v->mapped_pages[i / 8] & (1u << (i % 8)))) return 0;
    return 1;
}

static void mark_pages(NrVm *v, uintptr_t address, size_t length, int present) {
    if (!v->mapped_pages) return;
    size_t first = (address - (uintptr_t)v->base) / 4096;
    for (size_t i = first; i < first + length / 4096; ++i) {
        unsigned char bit = (unsigned char)(1u << (i % 8));
        if (present) v->mapped_pages[i / 8] |= bit;
        else v->mapped_pages[i / 8] &= (unsigned char)~bit;
    }
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
    if (flags & ~(uintptr_t)(1 | 2 | 0x10 | 0x20 | 0x800 | 0x4000 | 0x20000)) {
        fprintf(stderr, "mmap unsupported flags=0x%llx\n", (unsigned long long)flags); return -38;
    }
    if ((flags & 3) != 1 && (flags & 3) != 2) return -22;
    if (flags & 0x10) {
        if (!(flags & 2)) return -38;
        if (address & 4095) return -22;
        NrVm *v = NULL;
        for (unsigned i = 0; i < NR_VM_SLOTS; ++i)
            if (contains((uintptr_t)run->vm[i].base, run->vm[i].size, address, length)) { v = &run->vm[i]; break; }
        if (!v) return -38;
        if (v->section) return -38;
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
        mark_pages(v, address, length, 1);
        return (intptr_t)address;
    }
    NrVm *v = NULL;
    for (unsigned i = 0; i < NR_VM_SLOTS; ++i) if (!run->vm[i].base) { v = &run->vm[i]; break; }
    if (!v) { fputs("mmap guest region table exhausted\n", stderr); return -12; }
    if (flags & 0x20) {
        if ((flags & 1) || (args[2] & 4)) return -38;
        v->base = VirtualAlloc(NULL, length, MEM_RESERVE | (args[2] ? MEM_COMMIT : 0), protect(args[2]));
        if (!v->base) return -12;
        v->size = length;
        return (intptr_t)v->base;
    }
    if (flags & 1) {
        /* The original TSS path maps a small, already sized shared file at
           offset zero. Use a real Windows view so aliases/processes share
           pages and writes, rather than pretending a private copy is shared. */
        if ((args[2] & 4) || (offset & 65535)) return -38;
        if (fd >= 256 || !run->files[fd]) return -9;
        if (!(run->file_access[fd] & 1) || ((args[2] & 2) && !(run->file_access[fd] & 2))) return -13;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(run->files[fd], &size)) return -5;
        if (size.QuadPart <= 0 || offset >= (uint64_t)size.QuadPart ||
            args[1] > (uint64_t)size.QuadPart - offset) return -38;
        int writable = (run->file_access[fd] & 2) != 0;
        HANDLE section = CreateFileMappingW(run->files[fd], NULL, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, NULL);
        if (!section) return -5;
        void *base = MapViewOfFile(section, writable ? FILE_MAP_WRITE : FILE_MAP_READ,
                                   (DWORD)(offset >> 32), (DWORD)offset, (SIZE_T)args[1]);
        if (!base) { CloseHandle(section); return -12; }
        DWORD old;
        if (!VirtualProtect(base, length, protect(args[2]), &old)) {
            UnmapViewOfFile(base); CloseHandle(section); return -13;
        }
        v->base = base; v->size = length; v->section = section;
        return (intptr_t)base;
    }
    /* Private file mappings remain limited to immutable ELF dependency snapshots. */
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
    v->path = guest_path_from_handle(run, run->files[fd]);
    v->base = v->image.allocation;
    v->size = v->image.allocation_size;
    fprintf(stderr, "ELF dependency private snapshot sha256=%s base=0x%llx bytes=%zu\n", v->image.sha256,
            (unsigned long long)(uintptr_t)v->base, v->size);
    return (intptr_t)v->base;
}

#ifndef _WIN64
static intptr_t remapping(NrRun *run, const uintptr_t args[6]) {
    uintptr_t address = args[0], flags = args[3];
    size_t old_size = (size_t)args[1], new_size = (size_t)args[2];
    if (!address || !old_size || !new_size || (address & 4095) ||
        old_size > SIZE_MAX - 4095 || new_size > SIZE_MAX - 4095 ||
        flags != 1) return -22;
    old_size = (old_size + 4095) & ~(size_t)4095;
    new_size = (new_size + 4095) & ~(size_t)4095;
    NrVm *v = NULL;
    for (unsigned i = 0; i < NR_VM_SLOTS; ++i)
        if ((uintptr_t)run->vm[i].base == address) { v = &run->vm[i]; break; }
    if (!v || v->size != old_size || v->section || v->image.file) return -22;
    if (new_size <= old_size) {
        if (new_size < old_size && !VirtualFree((void *)(address + new_size), old_size - new_size, MEM_DECOMMIT))
            return -12;
        v->size = new_size;
        return (intptr_t)address;
    }
    void *replacement = VirtualAlloc(NULL, new_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!replacement) return -12;
    memcpy(replacement, (const void *)address, old_size);
    if (!VirtualFree((void *)address, 0, MEM_RELEASE)) {
        VirtualFree(replacement, 0, MEM_RELEASE);
        return -12;
    }
    if (v->mapped_pages) {
        HeapFree(GetProcessHeap(), 0, v->mapped_pages);
        v->mapped_pages = NULL;
    }
    v->base = replacement;
    v->size = new_size;
    return (intptr_t)replacement;
}
#endif

int nr_vm(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
#ifdef _WIN64
    enum {MMAP = 9, MPROTECT = 10, MUNMAP = 11, MADVISE = 28};
#else
    enum {MMAP = 192, MPROTECT = 125, MUNMAP = 91, MADVISE = 219, MREMAP = 163};
#endif
    if (number == MMAP) { *result = mapping(run, args); return 1; }
#ifndef _WIN64
    if (number == MREMAP) { *result = remapping(run, args); return 1; }
#endif
    if (number != MPROTECT && number != MUNMAP && number != MADVISE) return 0;
    uintptr_t address = args[0];
    size_t length = args[1];
    if ((address & 4095) || !length || length > SIZE_MAX - 4095) { *result = -22; return 1; }
    length = (length + 4095) & ~(size_t)4095;
    NrVm *v = NULL;
    for (unsigned i = 0; i < NR_VM_SLOTS; ++i)
        if (contains((uintptr_t)run->vm[i].base, run->vm[i].size, address, length)) { v = &run->vm[i]; break; }
    if (number == MADVISE) {
        if (!v || !mapped(v, address, length)) *result = -12;
        else if (args[2] != 4 || v->image.file || v->section) *result = -38;
        else if (!nr_memory(address, length, 1)) *result = -38;
        else if (!VirtualFree((void *)address, length, MEM_DECOMMIT)) *result = -12;
        else *result = VirtualAlloc((void *)address, length, MEM_COMMIT, PAGE_READWRITE) ? 0 : -12;
        return 1;
    }
    if (number == MUNMAP) {
        if (!v) { *result = -38; return 1; }
        if (address == (uintptr_t)v->base && length == v->size) {
            if (v->section) {
                if (!UnmapViewOfFile(v->base)) { *result = -22; return 1; }
                CloseHandle(v->section);
            } else if (v->image.file) nr_elf_close(&v->image);
            else if (!VirtualFree(v->base, 0, MEM_RELEASE)) { *result = -22; return 1; }
            if (v->source) CloseHandle(v->source);
            if (v->path) HeapFree(GetProcessHeap(), 0, v->path);
            if (v->mapped_pages) HeapFree(GetProcessHeap(), 0, v->mapped_pages);
            memset(v, 0, sizeof(*v));
            *result = 0;
        } else {
            if (v->section) { *result = -38; return 1; }
            if (!v->mapped_pages) {
                size_t bytes = (v->size / 4096 + 7) / 8;
                v->mapped_pages = HeapAlloc(GetProcessHeap(), 0, bytes);
                if (!v->mapped_pages) { *result = -12; return 1; }
                memset(v->mapped_pages, 255, bytes);
            }
            *result = VirtualFree((void *)address, length, MEM_DECOMMIT) ? 0 : -22;
            if (!*result) mark_pages(v, address, length, 0);
        }
        return 1;
    }
    int owned = v != NULL;
    const NrElf *images[] = {&run->program, &run->interpreter};
    for (unsigned i = 0; i < 2; ++i)
        if (contains((uintptr_t)images[i]->allocation, images[i]->allocation_size, address, length)) owned = 1;
    if (!owned) { *result = -12; return 1; }
    if (args[2] & ~7u) { *result = -38; return 1; }
    if (v && !mapped(v, address, length)) { *result = -12; return 1; }
    DWORD old;
    if (v && !v->image.file && !args[2]) {
        /* Reserved PROT_NONE pages are still guest mappings. Only committed
           pages need a Windows protection change; do not commit the rest. */
        uintptr_t cursor = address, end = address + length;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery((const void *)cursor, &info, sizeof(info))) { *result = -12; return 1; }
            size_t bytes = info.RegionSize - (cursor - (uintptr_t)info.BaseAddress);
            if (bytes > end - cursor) bytes = end - cursor;
            if (info.State == MEM_COMMIT) {
                if (!VirtualProtect((void *)cursor, bytes, PAGE_NOACCESS, &old)) { *result = -12; return 1; }
            } else if (info.State != MEM_RESERVE) { *result = -12; return 1; }
            cursor += bytes;
        }
        *result = 0; return 1;
    }
    if (v && !v->image.file && !v->section && args[2]) {
        uintptr_t cursor = address, end = address + length;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery((const void *)cursor, &info, sizeof(info))) { *result = -12; return 1; }
            size_t bytes = info.RegionSize - (cursor - (uintptr_t)info.BaseAddress);
            if (bytes > end - cursor) bytes = end - cursor;
            if (info.State == MEM_RESERVE) {
                if (!VirtualAlloc((void *)cursor, bytes, MEM_COMMIT, protect(args[2]))) { *result = -12; return 1; }
            } else if (info.State != MEM_COMMIT) { *result = -12; return 1; }
            cursor += bytes;
        }
    }
    *result = VirtualProtect((void *)address, length, protect(args[2]), &old) ? 0 : -12;
    return 1;
}

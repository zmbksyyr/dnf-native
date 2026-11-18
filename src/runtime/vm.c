#include "run.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN64
enum {MMAP = 9, MPROTECT = 10, MUNMAP = 11, MADVISE = 28, MSYNC = 26};
#else
enum {MMAP = 192, MPROTECT = 125, MUNMAP = 91, MADVISE = 219, MREMAP = 163, MSYNC = 144};
#endif

static DWORD protect(uintptr_t flags) {
    if (flags & 4) return flags & 2 ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (flags & 2) return PAGE_READWRITE;
    return flags & 1 ? PAGE_READONLY : PAGE_NOACCESS;
}

static DWORD view_protect(const NrVm *v, uintptr_t flags) {
    return v && v->private_view && (flags & 2) ? PAGE_WRITECOPY : protect(flags);
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
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative, -1, NULL, 0, NULL, NULL);
    if (!length) return NULL;
    char *guest = HeapAlloc(GetProcessHeap(), 0, (size_t)length + 1);
    if (!guest) return NULL;
    guest[0] = '/';
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, relative, -1,
                             guest + 1, length, NULL, NULL)) {
        HeapFree(GetProcessHeap(), 0, guest);
        return NULL;
    }
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

/* Discarded anonymous pages have a transient decommit while becoming zero
   pages with unchanged permissions. A legal concurrent reader may fault in
   that interval. Wait for restoration and retry at most once per reset epoch;
   actual guards, holes and permission violations continue to Linux signals. */
int nr_vm_retry_access(NrRun *run, const EXCEPTION_RECORD *fault) {
    if (fault->NumberParameters < 2) return 0;
    uintptr_t address = fault->ExceptionInformation[1], access = fault->ExceptionInformation[0];
    if (access != 0 && access != 1 && access != 8) return 0;
    EnterCriticalSection(&run->memory_lock);
    NrThread *thread = nr_current();
    int retry = 0;
    if (thread->vm_retry_generation != run->vm_reset_generation) {
        for (unsigned i = 0; i < run->vm_count; ++i) {
            const NrVm *v = &run->vm[i];
            if (!contains((uintptr_t)v->base, v->size, address, 1) || v->image.file || v->section ||
                !mapped(v, address & ~(uintptr_t)4095, 4096)) continue;
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery((const void *)address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
            DWORD allowed = access == 1 ? PAGE_READWRITE | PAGE_EXECUTE_READWRITE :
                access == 8 ? PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE :
                PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
            retry = (info.Protect & allowed) != 0;
            break;
        }
        thread->vm_retry_generation = run->vm_reset_generation;
    }
    LeaveCriticalSection(&run->memory_lock);
    return retry;
}

static intptr_t discard_anonymous(NrRun *run, NrVm *v, uintptr_t address, size_t length) {
    uintptr_t cursor = address, end = address + length;
    intptr_t result = 0;
    while (cursor < end) {
        if (!mapped(v, cursor, 4096)) { result = -12; cursor += 4096; continue; }
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)cursor, &info, sizeof(info))) return -12;
        size_t bytes = info.RegionSize - (cursor - (uintptr_t)info.BaseAddress);
        if (bytes > end - cursor) bytes = end - cursor;
        if (v->mapped_pages) {
            size_t present = 4096;
            while (present < bytes && mapped(v, cursor + present, 4096)) present += 4096;
            bytes = present;
        }
        if (info.State == MEM_COMMIT) {
            ++run->vm_reset_generation;
            if (!VirtualFree((void *)cursor, bytes, MEM_DECOMMIT) ||
                !VirtualAlloc((void *)cursor, bytes, MEM_COMMIT, info.Protect)) return -12;
            if ((info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                !FlushInstructionCache(GetCurrentProcess(), (void *)cursor, bytes)) return -5;
        } else if (info.State != MEM_RESERVE) return -12;
        cursor += bytes;
    }
    return result;
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
        for (unsigned i = 0; i < run->vm_count; ++i)
            if (contains((uintptr_t)run->vm[i].base, run->vm[i].size, address, length)) { v = &run->vm[i]; break; }
        if (!v) return -38;
        if (v->section) return -38;
        if (!(flags & 0x20) && (!v->image.file || fd >= NR_FD_SLOTS || !run->files[fd] || !v->source)) return -38;
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
    unsigned slot = run->vm_count;
    for (unsigned i = 0; i < run->vm_count; ++i) if (!run->vm[i].base) { slot = i; break; }
    if (slot == NR_VM_SLOTS) {
        size_t reserved = 0, live_pages = 0, partial = 0;
        for (unsigned i = 0; i < run->vm_count; ++i) {
            NrVm *region = &run->vm[i]; reserved += region->size;
            if (region->mapped_pages) {
                ++partial;
                for (size_t page = 0; page < region->size / 4096; ++page)
                    live_pages += (region->mapped_pages[page / 8] >> (page % 8)) & 1;
            } else live_pages += region->size / 4096;
        }
        fprintf(stderr, "mmap guest region table exhausted slots=%u partial=%zu reserved=%zu live=%zu\n",
                NR_VM_SLOTS, partial, reserved, live_pages * 4096);
        return -12;
    }
    NrVm *v = &run->vm[slot];
    if (flags & 0x20) {
        if (flags & 1) return -38;
        v->base = VirtualAlloc(NULL, length, MEM_RESERVE | (args[2] ? MEM_COMMIT : 0), protect(args[2]));
        if (!v->base) return -12;
        v->size = length;
        if (slot == run->vm_count) ++run->vm_count;
        return (intptr_t)v->base;
    }
    if (flags & 1) {
        /* The original TSS path maps a small, already sized shared file at
           offset zero. Use a real Windows view so aliases/processes share
           pages and writes, rather than pretending a private copy is shared. */
        if ((args[2] & 4) || (offset & 65535)) return -38;
        if (fd >= NR_FD_SLOTS || !run->files[fd]) return -9;
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
        if (writable && !DuplicateHandle(GetCurrentProcess(), run->files[fd], GetCurrentProcess(),
                                         &v->source, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            UnmapViewOfFile(base); CloseHandle(section); return -5;
        }
        v->base = base; v->view_base = base; v->size = length; v->section = section;
        v->path = guest_path_from_handle(run, run->files[fd]);
        v->file_offset = offset;
        v->file_inode = nr_file_inode(run->files[fd]);
        if (slot == run->vm_count) ++run->vm_count;
        return (intptr_t)base;
    }
    if (fd >= NR_FD_SLOTS || !run->files[fd]) return -9;
    if (!(run->file_access[fd] & 1)) return -13;
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(run->files[fd], &file_size)) return -5;
    if (file_size.QuadPart <= 0 || offset >= (uint64_t)file_size.QuadPart) return -38;
    uint64_t available = (uint64_t)file_size.QuadPart - offset;
    size_t delta = (size_t)(offset & 65535);
    size_t file_bytes = available < length ? (size_t)available : length;
    if (file_bytes > SIZE_MAX - delta) return -12;
    uint64_t view_offset = offset - delta;
    HANDLE section = CreateFileMappingW(run->files[fd], NULL, PAGE_WRITECOPY, 0, 0, NULL);
    if (!section) return -5;
    void *view = MapViewOfFile(section, FILE_MAP_COPY, (DWORD)(view_offset >> 32),
                               (DWORD)view_offset, file_bytes + delta);
    if (!view) { CloseHandle(section); return -12; }
    unsigned char *base = (unsigned char *)view + delta;
    int elf = !offset && file_bytes >= 4 && !memcmp(base, "\177ELF", 4);
    if (!elf) {
        /* A page containing EOF has a zero tail. Whole pages beyond EOF need
           SIGBUS support. ELF reservations separately include their BSS. */
        if (length - 4096 >= available) { UnmapViewOfFile(view); CloseHandle(section); return -38; }
        /* Data mappings use real Windows copy-on-write pages, even with a
           readonly fd. Mapping neither advances the fd nor writes its file. */
        if (args[2] & 4) { UnmapViewOfFile(view); CloseHandle(section); return -38; }
        DWORD old;
        DWORD permission = args[2] & 2 ? PAGE_WRITECOPY : protect(args[2]);
        if (!VirtualProtect(base, length, permission, &old)) {
            UnmapViewOfFile(view); CloseHandle(section); return -13;
        }
        v->base = base; v->size = length; v->section = section;
        v->view_base = view; v->private_view = 1;
        v->path = guest_path_from_handle(run, run->files[fd]);
        v->file_offset = offset;
        v->file_inode = nr_file_inode(run->files[fd]);
        if (slot == run->vm_count) ++run->vm_count;
        return (intptr_t)base;
    }
    UnmapViewOfFile(view); CloseHandle(section);
    /* Keep the existing eager ELF snapshot/adaptation used by guest ld.so. */
    if (offset) return -38;
    wchar_t path[32768];
    DWORD n = GetFinalPathNameByHandleW(run->files[fd], path, 32768, FILE_NAME_NORMALIZED);
    if (!n || n >= 32768) return -5;
    const char *error;
    if (!nr_elf_open(&v->image, path, NULL, 0, &error)) {
        char *guest = guest_path_from_handle(run, run->files[fd]);
        fprintf(stderr, "private mmap rejected path=%s fd=%llu bytes=%zu offset=0x%llx reason=%s\n",
                guest ? guest : "[unknown]", (unsigned long long)fd, length,
                (unsigned long long)offset, error ? error : "not an ELF dependency");
        if (guest) HeapFree(GetProcessHeap(), 0, guest);
        return -38;
    }
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
    if (slot == run->vm_count) ++run->vm_count;
    fprintf(stderr, "ELF dependency private snapshot sha256=%s base=0x%llx bytes=%zu\n", v->image.sha256,
            (unsigned long long)(uintptr_t)v->base, v->size);
    nr_log_image(&v->image, v->path ? v->path : "[dependency]");
    return (intptr_t)v->base;
}

static int overlaps(uintptr_t address, size_t length, uintptr_t base, size_t size) {
    return base && size && base < address + length && address < base + size;
}

static intptr_t unmapping(NrRun *run, uintptr_t address, size_t length) {
    const NrElf *images[] = {&run->program, &run->interpreter};
    for (unsigned i = 0; i < 2; ++i)
        if (overlaps(address, length, (uintptr_t)images[i]->allocation, images[i]->allocation_size)) return -38;
    if (overlaps(address, length, run->stack_low, run->stack_high - run->stack_low) ||
        overlaps(address, length, run->heap_base, run->heap_committed)) return -38;

    /* Allocate all needed page masks before removing anything. Linux permits
       one unmap to span neighboring mappings and unmapped holes. */
    uintptr_t end = address + length;
    for (unsigned i = 0; i < run->vm_count; ++i) {
        NrVm *v = &run->vm[i];
        uintptr_t first = (uintptr_t)v->base, last = first + v->size;
        if (!overlaps(address, length, first, v->size) || (address <= first && end >= last) || v->mapped_pages) continue;
        size_t pages = v->size / 4096, bytes = (pages + 7) / 8;
        v->mapped_pages = HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!v->mapped_pages) return -12;
        memset(v->mapped_pages, 255, bytes);
        if (pages % 8) v->mapped_pages[bytes - 1] = (unsigned char)((1u << (pages % 8)) - 1);
    }
    for (unsigned i = 0; i < run->vm_count; ++i) {
        NrVm *v = &run->vm[i];
        uintptr_t first = (uintptr_t)v->base, last = first + v->size;
        if (!overlaps(address, length, first, v->size)) continue;
        if (address > first || end < last) {
            uintptr_t start = address > first ? address : first;
            size_t bytes = (end < last ? end : last) - start;
            DWORD old;
            /* Windows cannot decommit part of a file view. Inaccessible pages
               plus the guest bitmap preserve unmap semantics until final release. */
            if (v->section ? !VirtualProtect((void *)start, bytes, PAGE_NOACCESS, &old) :
                             !VirtualFree((void *)start, bytes, MEM_DECOMMIT)) return -22;
            mark_pages(v, start, bytes, 0);
            size_t bitmap_bytes = (v->size / 4096 + 7) / 8;
            size_t remaining = 0;
            while (remaining < bitmap_bytes && !v->mapped_pages[remaining]) ++remaining;
            if (remaining < bitmap_bytes) continue;
        }
        if (v->section) {
            if (!UnmapViewOfFile(v->view_base)) return -22;
            CloseHandle(v->section);
        } else if (v->image.file) nr_elf_close(&v->image);
        else if (!VirtualFree(v->base, 0, MEM_RELEASE)) return -22;
        if (v->source) CloseHandle(v->source);
        if (v->path) HeapFree(GetProcessHeap(), 0, v->path);
        if (v->mapped_pages) HeapFree(GetProcessHeap(), 0, v->mapped_pages);
        memset(v, 0, sizeof(*v));
    }
    while (run->vm_count && !run->vm[run->vm_count - 1].base) --run->vm_count;
    return 0;
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
    for (unsigned i = 0; i < run->vm_count; ++i)
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

static int vm_operation(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
    if (number == MMAP) { *result = mapping(run, args); return 1; }
#ifndef _WIN64
    if (number == MREMAP) { *result = remapping(run, args); return 1; }
#endif
    if (number != MPROTECT && number != MUNMAP && number != MADVISE && number != MSYNC) return 0;
    uintptr_t address = args[0];
    size_t length = args[1];
    if (number == MSYNC) {
        if ((args[2] & ~(uintptr_t)7) || ((args[2] & 5) == 5) || (address & 4095) || length > SIZE_MAX - 4095) {
            *result = -22; return 1;
        }
        if (args[2] & 2) { fputs("msync unsupported MS_INVALIDATE\n", stderr); *result = -38; return 1; }
        if (!length) { *result = 0; return 1; }
    }
    if (number == MADVISE && !(address & 4095) && !length && args[2] == 4) { *result = 0; return 1; }
    if ((address & 4095) || !length || length > SIZE_MAX - 4095) { *result = -22; return 1; }
    length = (length + 4095) & ~(size_t)4095;
    if (address > UINTPTR_MAX - length) { *result = -22; return 1; }
    if (number == MUNMAP) { *result = unmapping(run, address, length); return 1; }
    NrVm *v = NULL;
    for (unsigned i = 0; i < run->vm_count; ++i)
        if (contains((uintptr_t)run->vm[i].base, run->vm[i].size, address, length)) { v = &run->vm[i]; break; }
    if (number == MSYNC) {
        if (!v || !mapped(v, address, length)) { *result = -12; return 1; }
        *result = 0;
        /* Private/anonymous pages have no writeback. Windows already tracks
           dirty shared pages for asynchronous writeback, as Linux MS_ASYNC does. */
        if (v->section && !v->private_view && (args[2] & 4)) {
            if (!FlushViewOfFile((const void *)address, length) || (v->source && !FlushFileBuffers(v->source)))
                *result = -5;
        }
        return 1;
    }
    if (number == MADVISE) {
        if (!v) *result = -12;
        else if (args[2] != 4 || v->image.file || v->section) *result = -38;
        else *result = discard_anonymous(run, v, address, length);
        return 1;
    }
    int owned = v != NULL;
    const NrElf *images[] = {&run->program, &run->interpreter};
    for (unsigned i = 0; i < 2; ++i)
        if (contains((uintptr_t)images[i]->allocation, images[i]->allocation_size, address, length)) owned = 1;
    if (!owned) { *result = -12; return 1; }
    if (args[2] & ~7u) { *result = -38; return 1; }
    if (v && !mapped(v, address, length)) { *result = -12; return 1; }
    if (v && v->private_view && (args[2] & 4)) { *result = -38; return 1; }
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
    *result = VirtualProtect((void *)address, length, view_protect(v, args[2]), &old) ? 0 : -12;
    return 1;
}

int nr_vm(NrRun *run, uintptr_t number, const uintptr_t args[6], intptr_t *result) {
    if (number != MMAP && number != MPROTECT && number != MUNMAP && number != MADVISE && number != MSYNC
#ifndef _WIN64
        && number != MREMAP
#endif
    ) return 0;
    /* Threads share one Linux address space. Reserve, publish and release its
       records together so concurrent mmap/munmap cannot claim the same slot. */
    EnterCriticalSection(&run->memory_lock);
    run->memory_region_count = run->memory_region_cursor = 0;
    int handled = vm_operation(run, number, args, result);
    run->memory_region_count = run->memory_region_cursor = 0;
    LeaveCriticalSection(&run->memory_lock);
    return handled;
}

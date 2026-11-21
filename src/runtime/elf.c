void nr_site_cache_reset(void);

#include "elf.h"
#include <bcrypt.h>
#include <string.h>
#include <limits.h>

#define NR_MAX_IMAGE (512u * 1024u * 1024u)
#define NR_PT_LOAD 1
#define NR_PT_INTERP 3

#ifdef _WIN64
_Static_assert(sizeof(NrElfHeader) == 64, "ELF64 header layout");
_Static_assert(sizeof(NrElfProgram) == 56, "ELF64 program layout");
#else
_Static_assert(sizeof(NrElfHeader) == 52, "ELF32 header layout");
_Static_assert(sizeof(NrElfProgram) == 32, "ELF32 program layout");
#endif

static int range(size_t size, uint64_t offset, uint64_t count) {
    return offset <= size && count <= size - offset;
}

static int file_hash(NrElf *elf) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    DWORD object_size = 0, written = 0;
    unsigned char *object = NULL;
    unsigned char buffer[64 * 1024];
    int ok = 0;
    static const char hex[] = "0123456789abcdef";
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        goto done;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &written, 0) < 0)
        goto done;
    object = HeapAlloc(GetProcessHeap(), 0, object_size);
    if (!object || BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0) < 0)
        goto done;
    /* Hash through the original file handle instead of touching every byte
       of the immutable mapping.  ELF headers and load segments still use the
       mapping below, while debug/symbol data stays file-backed and cold. */
    LARGE_INTEGER origin;
    origin.QuadPart = 0;
    if (!elf->file_handle || !SetFilePointerEx(elf->file_handle, origin, NULL, FILE_BEGIN))
        goto done;
    size_t remaining = elf->file_size;
    while (remaining) {
        DWORD want = remaining > sizeof(buffer) ? (DWORD)sizeof(buffer) : (DWORD)remaining;
        DWORD got = 0;
        if (!ReadFile(elf->file_handle, buffer, want, &got, NULL) || !got || got > remaining)
            goto done;
        if (BCryptHashData(hash, buffer, got, 0) < 0)
            goto done;
        remaining -= got;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        goto done;
    for (unsigned i = 0; i < sizeof(digest); ++i) {
        elf->sha256[2 * i] = hex[digest[i] >> 4];
        elf->sha256[2 * i + 1] = hex[digest[i] & 15];
    }
    elf->sha256[64] = 0;
    ok = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    return ok;
}

int nr_elf_open(NrElf *elf, const wchar_t *path, const char *expected_hash, int require_entry, const char **error) {
    HANDLE input = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    memset(elf, 0, sizeof(*elf));
    *error = "cannot read ELF";
    input = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (input == INVALID_HANDLE_VALUE || !GetFileSizeEx(input, &size) ||
        size.QuadPart < (LONGLONG)sizeof(NrElfHeader) || size.QuadPart > NR_MAX_IMAGE)
        goto fail;
    elf->file_size = (size_t)size.QuadPart;
    /* Keep immutable input bytes in a shared, read-only file view instead of
       a second private heap copy in every service process. Retaining the
       read-only handle also prevents writes while the ELF is in use. */
    HANDLE section = CreateFileMappingW(input, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!section) goto fail;
    elf->file = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(section);
    if (!elf->file) goto fail;
    elf->file_handle = input;
    input = INVALID_HANDLE_VALUE;
    *error = "ELF SHA-256 mismatch";
    if (!file_hash(elf) || (expected_hash && (strlen(expected_hash) != 64 || _stricmp(elf->sha256, expected_hash))))
        goto fail;
    NrElfHeader *h = elf->header = (NrElfHeader *)elf->file;
    *error = "invalid ELF architecture or header";
    if (memcmp(h->ident, "\177ELF", 4) || h->ident[5] != 1 || h->ident[6] != 1 ||
        h->version != 1 || h->ehsize != sizeof(*h) || h->phentsize != sizeof(NrElfProgram) ||
        !h->phnum || h->phnum > 1024 || (h->type != 2 && h->type != 3))
        goto fail;
#ifdef _WIN64
    if (h->ident[4] != 2 || h->machine != 62) goto fail;
#else
    if (h->ident[4] != 1 || h->machine != 3) goto fail;
#endif
    if (!range(elf->file_size, h->phoff, (uint64_t)h->phnum * sizeof(NrElfProgram)))
        goto fail;
    elf->programs = (NrElfProgram *)(elf->file + h->phoff);
    uint64_t low = UINT64_MAX, high = 0;
    int entry_found = 0;
    for (unsigned i = 0; i < h->phnum; ++i) {
        NrElfProgram *p = &elf->programs[i];
        if (p->type == NR_PT_INTERP) {
            *error = "invalid PT_INTERP";
            if (!p->filesz || !range(elf->file_size, p->offset, p->filesz) ||
                elf->file[p->offset + p->filesz - 1] != 0)
                goto fail;
        }
        if (p->type != NR_PT_LOAD || !p->memsz) continue;
        *error = "invalid PT_LOAD bounds or alignment";
        if (p->filesz > p->memsz || !range(elf->file_size, p->offset, p->filesz) ||
            p->vaddr > UINTPTR_MAX || p->memsz > UINTPTR_MAX - p->vaddr ||
            (p->align > 1 && ((p->align & (p->align - 1)) ||
             (p->vaddr % p->align != p->offset % p->align))))
            goto fail;
        if (p->vaddr < low) low = p->vaddr;
        uint64_t end = (uint64_t)p->vaddr + p->memsz;
        if (end > high) high = end;
        if ((p->flags & 1) && h->entry >= p->vaddr && h->entry < end)
            entry_found = 1;
    }
    *error = "ELF has no bounded executable entry";
    /* e_entry may be zero for a DSO; programs and PT_INTERP need a real entry. */
    if (low == UINT64_MAX || high <= low || high - low > NR_MAX_IMAGE ||
        (require_entry && !h->entry) || (!entry_found && (require_entry || h->type != 3 || h->entry)))
        goto fail;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uint64_t mask = info.dwAllocationGranularity - 1;
    low &= ~mask;
    if (high > UINTPTR_MAX - mask) goto fail;
    high = (high + mask) & ~mask;
    if (high - low > NR_MAX_IMAGE) goto fail;
    elf->low = (uintptr_t)low;
    elf->allocation_size = (size_t)(high - low);
    *error = NULL;
    return 1;
fail:
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    nr_elf_close(elf);
    return 0;
}

static DWORD protection(unsigned flags) {
    if (flags & 1) return flags & 2 ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (flags & 2) return PAGE_READWRITE;
    return flags & 4 ? PAGE_READONLY : PAGE_NOACCESS;
}

int nr_elf_map(NrElf *elf, const char **error) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size_t page_size = info.dwPageSize;
    size_t pages = elf->allocation_size / page_size;
    unsigned char *flags = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pages);
    *error = "cannot reserve original ELF address range";
    if (!flags) return 0;
    void *wanted = elf->header->type == 2 ? (void *)elf->low : NULL;
    if (elf->header->type == 2 && !wanted) goto fail;
    elf->allocation = VirtualAlloc(wanted, elf->allocation_size,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!elf->allocation) goto fail;
    elf->bias = (uintptr_t)elf->allocation - elf->low;
    elf->entry = elf->bias + (uintptr_t)elf->header->entry;
    for (unsigned i = 0; i < elf->header->phnum; ++i) {
        NrElfProgram *p = &elf->programs[i];
        if (p->type != NR_PT_LOAD || !p->memsz) continue;
        size_t start = (size_t)(p->vaddr - elf->low);
        memcpy(elf->allocation + start, elf->file + p->offset, (size_t)p->filesz);
        size_t first = start / page_size;
        size_t last = (start + (size_t)p->memsz + page_size - 1) / page_size;
        for (size_t page = first; page < last; ++page)
            flags[page] |= (unsigned char)p->flags & 7;
    }
    *error = "cannot protect ELF pages";
    for (size_t start = 0; start < pages;) {
        size_t end = start + 1;
        while (end < pages && flags[end] == flags[start]) ++end;
        DWORD old;
        if (!VirtualProtect(elf->allocation + start * page_size, (end - start) * page_size,
                             protection(flags[start]), &old))
            goto fail;
        start = end;
    }
    if (!FlushInstructionCache(GetCurrentProcess(), elf->allocation, elf->allocation_size))
        goto fail;
    HeapFree(GetProcessHeap(), 0, flags);
    *error = NULL;
    return 1;
fail:
    if (elf->allocation) VirtualFree(elf->allocation, 0, MEM_RELEASE);
    elf->allocation = NULL;
    HeapFree(GetProcessHeap(), 0, flags);
    return 0;
}

void nr_elf_close(NrElf *elf) {
    while (elf->tls_pages) {
        NrCodePage *next = elf->tls_pages->next;
        VirtualFree(elf->tls_pages, 0, MEM_RELEASE);
        elf->tls_pages = next;
    }
    if (elf->allocation) VirtualFree(elf->allocation, 0, MEM_RELEASE);
    if (elf->file) UnmapViewOfFile(elf->file);
    if (elf->file_handle) CloseHandle(elf->file_handle);
    nr_site_cache_reset();
    if (elf->traps) HeapFree(GetProcessHeap(), 0, elf->traps);
    memset(elf, 0, sizeof(*elf));
}

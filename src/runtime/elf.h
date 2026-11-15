#ifndef DNF_NATIVE_ELF_H
#define DNF_NATIVE_ELF_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t name, type;
#ifdef _WIN64
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t align, entsize;
#else
    uint32_t flags, addr, offset, size, link, info, align, entsize;
#endif
} NrElfSection;

typedef struct {
    uintptr_t pc;
    unsigned char original[2];
    unsigned char tls;
} NrTrap;

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
#ifdef _WIN64
    uint64_t entry, phoff, shoff;
#else
    uint32_t entry, phoff, shoff;
#endif
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} NrElfHeader;

typedef struct {
    uint32_t type;
#ifdef _WIN64
    uint32_t flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
#else
    uint32_t offset, vaddr, paddr, filesz, memsz, flags, align;
#endif
} NrElfProgram;

typedef struct NrCodePage {
    struct NrCodePage *next;
    size_t used;
    unsigned char code[4096 - sizeof(void *) - sizeof(size_t)];
} NrCodePage;

typedef struct {
    unsigned char *file;
    HANDLE file_handle;
    size_t file_size;
    NrElfHeader *header;
    NrElfProgram *programs;
    unsigned char *allocation;
    size_t allocation_size;
    uintptr_t low, bias, entry;
    char sha256[65];
    NrTrap *traps;
    size_t trap_count;
    NrCodePage *tls_pages;
} NrElf;

int nr_elf_open(NrElf *elf, const wchar_t *path, const char *expected_hash, int require_entry, const char **error);
int nr_elf_map(NrElf *elf, const char **error);
void nr_elf_close(NrElf *elf);
int nr_adapt(NrElf *elf, const char **error);
const NrTrap *nr_syscall_site(const NrElf *elf, uintptr_t pc);

#endif

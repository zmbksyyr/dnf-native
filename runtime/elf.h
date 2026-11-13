#ifndef DNF_NATIVE_ELF_H
#define DNF_NATIVE_ELF_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

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

typedef struct {
    unsigned char *file;
    size_t file_size;
    NrElfHeader *header;
    NrElfProgram *programs;
    unsigned char *allocation;
    size_t allocation_size;
    uintptr_t low, bias, entry;
    char sha256[65];
} NrElf;

int nr_elf_open(NrElf *elf, const wchar_t *path, const char *expected_hash, const char **error);
int nr_elf_map(NrElf *elf, const char **error);
void nr_elf_close(NrElf *elf);

#endif

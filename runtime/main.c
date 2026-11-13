#include "elf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wmain(int argc, wchar_t **argv) {
    if (argc != 4 || wcscmp(argv[1], L"--map")) {
        fputs("usage: host --map ELF SHA256\n", stderr);
        return 2;
    }
    char expected[65];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[3], -1,
                            expected, sizeof(expected), NULL, NULL) != 65) {
        fputs("invalid SHA256 argument\n", stderr);
        return 2;
    }
    NrElf elf;
    const char *error;
    if (!nr_elf_open(&elf, argv[2], expected, &error)) {
        fprintf(stderr, "ELF validation failed: %s\n", error);
        return 1;
    }
    if (!nr_elf_map(&elf, &error)) {
        fprintf(stderr, "ELF mapping failed: %s (Win32 %lu)\n", error, GetLastError());
        nr_elf_close(&elf);
        return 1;
    }
    printf("{\"mapped\":true,\"bits\":%u,\"entry\":\"0x%llx\",\"bias\":\"0x%llx\","
           "\"reserved_bytes\":%zu,\"sha256\":\"%s\",\"executed\":false}\n",
           (unsigned)(sizeof(void *) * 8), (unsigned long long)elf.entry,
           (unsigned long long)elf.bias, elf.allocation_size, elf.sha256);
    nr_elf_close(&elf);
    return 0;
}

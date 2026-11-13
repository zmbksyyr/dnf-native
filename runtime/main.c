#include "elf.h"
#include "run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wmain(int argc, wchar_t **argv) {
    wchar_t *root = NULL, *ld_hash = NULL, *cwd = NULL;
    while (argc >= 3) {
        if (argc >= 4 && !wcscmp(argv[1], L"--root")) {
            root = argv[2]; ld_hash = argv[3]; argc -= 3; argv += 3;
        } else if (!wcscmp(argv[1], L"--cwd")) {
            cwd = argv[2]; argc -= 2; argv += 2;
        } else break;
    }
    int execute = argc >= 5 && !wcscmp(argv[1], L"--run");
    if (!execute && (argc != 4 || wcscmp(argv[1], L"--map"))) {
        fputs("usage: host --map ELF SHA256\n       host [--root ROOT INTERPRETER_SHA256] [--cwd GUEST_DIR] --run ELF SHA256 GUEST_ARGV0 [ARGS...]\n", stderr);
        return 2;
    }
    char expected[65];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[3], -1,
                            expected, sizeof(expected), NULL, NULL) != 65) {
        fputs("invalid SHA256 argument\n", stderr);
        return 2;
    }
    NrRun run = {0};
    strcpy(run.cwd, "/");
    if (cwd && (cwd[0] != L'/' || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cwd, -1, run.cwd, sizeof(run.cwd), NULL, NULL))) return 2;
    if (root && (!GetFullPathNameW(root, 32768, run.root, NULL) || wcslen(run.root) >= 32767)) return 2;
    NrElf *elf = &run.program;
    const char *error;
    if (!nr_elf_open(elf, argv[2], expected, &error)) {
        fprintf(stderr, "ELF validation failed: %s\n", error);
        return 1;
    }
    if (!nr_elf_map(elf, &error)) {
        fprintf(stderr, "ELF mapping failed: %s (Win32 %lu)\n", error, GetLastError());
        nr_elf_close(elf);
        return 1;
    }
    if (execute) {
        for (unsigned i = 0; i < elf->header->phnum; ++i) {
            if (elf->programs[i].type == 3) {
                wchar_t path[32768];
                char hash[65];
                if (!ld_hash || nr_path(&run, (const char *)elf->file + elf->programs[i].offset, path) ||
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ld_hash, -1, hash, sizeof(hash), NULL, NULL) != 65) {
                    fputs("PT_INTERP requires --root ROOT INTERPRETER_SHA256\n", stderr);
                    nr_elf_close(elf); return 1;
                }
                if (!nr_elf_open(&run.interpreter, path, hash, &error) || !nr_elf_map(&run.interpreter, &error) ||
                    !nr_adapt(&run.interpreter, &error)) {
                    fprintf(stderr, "interpreter load failed: %s\n", error ? error : "Windows memory operation failed");
                    nr_elf_close(&run.interpreter); nr_elf_close(elf); return 1;
                }
                break;
            }
        }
        if (!nr_adapt(elf, &error)) {
            fprintf(stderr, "adaptation failed: %s\n", error ? error : "Windows memory operation failed");
            nr_elf_close(elf);
            return 1;
        }
        run.argc = argc - 4;
        run.argv = calloc((size_t)run.argc, sizeof(char *));
        if (!run.argv) return 1;
        for (int i = 0; i < run.argc; ++i) {
            int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i + 4], -1, NULL, 0, NULL, NULL);
            if (!bytes || !(run.argv[i] = malloc((size_t)bytes))) return 1;
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i + 4], -1, run.argv[i], bytes, NULL, NULL)) return 1;
        }
        int status = nr_run(&run);
        for (int i = 0; i < run.argc; ++i) free(run.argv[i]);
        free(run.argv);
        nr_elf_close(elf);
        return status;
    }
    printf("{\"mapped\":true,\"bits\":%u,\"entry\":\"0x%llx\",\"bias\":\"0x%llx\","
           "\"reserved_bytes\":%zu,\"sha256\":\"%s\",\"executed\":false}\n",
           (unsigned)(sizeof(void *) * 8), (unsigned long long)elf->entry,
           (unsigned long long)elf->bias, elf->allocation_size, elf->sha256);
    nr_elf_close(elf);
    return 0;
}

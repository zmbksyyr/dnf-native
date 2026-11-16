#include "elf.h"
#include "run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *utf8(const wchar_t *value) {
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    char *text = bytes ? malloc((size_t)bytes) : NULL;
    if (!text) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, text, bytes, NULL, NULL)) {
        free(text); return NULL;
    }
    return text;
}

int wmain(int argc, wchar_t **argv) {
    static NrRun run;
    wchar_t *root = NULL, *ld_hash = NULL, *cwd = NULL;
    while (argc >= 3) {
        if (argc >= 4 && !wcscmp(argv[1], L"--root")) {
            root = argv[2]; ld_hash = argv[3]; argc -= 3; argv += 3;
        } else if (!wcscmp(argv[1], L"--cwd")) {
            cwd = argv[2]; argc -= 2; argv += 2;
        } else if (!wcscmp(argv[1], L"--env")) {
            const wchar_t *equals = wcschr(argv[2], L'=');
            if (!equals || equals == argv[2] || run.environment_count == 32) return 2;
            char *value = utf8(argv[2]);
            if (!value) return 1;
            run.environment[run.environment_count++] = value;
            argc -= 2; argv += 2;
        } else break;
    }
    int execute = argc >= 5 && !wcscmp(argv[1], L"--run");
    if (!execute && (argc != 4 || wcscmp(argv[1], L"--map"))) {
        fputs("usage: host --map ELF SHA256\n       host [--root ROOT INTERPRETER_SHA256] [--cwd GUEST_DIR] [--env NAME=VALUE] --run ELF SHA256 GUEST_ARGV0 [ARGS...]\n", stderr);
        return 2;
    }
    char expected[65];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[3], -1,
                            expected, sizeof(expected), NULL, NULL) != 65) {
        fputs("invalid SHA256 argument\n", stderr);
        return 2;
    }
    strcpy(run.cwd, "/");
    if (cwd && (cwd[0] != L'/' || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cwd, -1, run.cwd, sizeof(run.cwd), NULL, NULL))) return 2;
    if (root && (!GetFullPathNameW(root, 32768, run.root, NULL) || wcslen(run.root) >= 32767)) return 2;
    NrElf *elf = &run.program;
    const char *error;
    if (!nr_elf_open(elf, argv[2], expected, execute, &error)) {
        fprintf(stderr, "ELF validation failed: %s\n", error);
        return 1;
    }
    if (!nr_elf_map(elf, &error)) {
        fprintf(stderr, "ELF mapping failed: %s (Win32 %lu)\n", error, GetLastError());
        nr_elf_close(elf);
        return 1;
    }
    if (execute) {
        if (!nr_game_configure(&run, &error)) {
            fprintf(stderr, "Game configuration failed: %s\n", error);
            nr_elf_close(elf);
            return 1;
        }
        if (!nr_pvf_encoding(&run, &error)) {
            fprintf(stderr, "PVF encoding selection failed: %s\n", error ? error : "Windows memory operation failed");
            nr_elf_close(elf);
            return 1;
        }
        if (!nr_thread_init(&run)) return 1;
        for (unsigned i = 0; i < elf->header->phnum; ++i) {
            if (elf->programs[i].type == 3) {
                wchar_t path[32768];
                char hash[65];
                if (!ld_hash || nr_path(&run, (const char *)elf->file + elf->programs[i].offset, path) ||
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ld_hash, -1, hash, sizeof(hash), NULL, NULL) != 65) {
                    fputs("PT_INTERP requires --root ROOT INTERPRETER_SHA256\n", stderr);
                    nr_elf_close(elf); return 1;
                }
                if (!nr_elf_open(&run.interpreter, path, hash, 1, &error) || !nr_elf_map(&run.interpreter, &error) ||
                    !nr_adapt(&run.interpreter, &error)) {
                    fprintf(stderr, "interpreter load failed: %s\n", error ? error : "Windows memory operation failed");
                    nr_elf_close(&run.interpreter); nr_elf_close(elf); return 1;
                }
                nr_log_image(&run.interpreter, (const char *)elf->file + elf->programs[i].offset);
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
            if (!(run.argv[i] = utf8(argv[i + 4]))) return 1;
        }
        nr_log_image(elf, run.argv[0]);
        int status = nr_run(&run);
        for (int i = 0; i < run.argc; ++i) free(run.argv[i]);
        free(run.argv);
        for (int i = 0; i < run.environment_count; ++i) free(run.environment[i]);
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

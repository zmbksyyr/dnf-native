#include "run.h"
#include "../game_levels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Application rules, independently checked against the pinned ELF's complete
   instructions. Disk inputs remain unchanged. Do not import replacement ELFs:
   the supplied level variants also change unrelated gameplay and logging. */
#ifndef _WIN64
typedef struct {
    uintptr_t pc;
    unsigned char bytes[10], length, operand, previous;
} LevelSite;

static const LevelSite levels[] = {
    {0x08360c38, {0x83,0x7d,0xf0,0x46}, 4,3,0}, /* SP initialization */
    {0x08360c76, {0x83,0x7d,0xf4,0x46}, 4,3,0},
    {0x08360cc1, {0x83,0x7d,0x0c,0x46}, 4,3,0}, /* SP lookup */
    {0x08662f53, {0x83,0xf8,0x46}, 3,2,0},       /* level-up events */
    {0x086630f0, {0x83,0x7d,0xd0,0x46}, 4,3,0}, /* level-up loop */
    {0x086638f4, {0x83,0xf8,0x46}, 3,2,0},
    {0x08665d24, {0xc7,0x44,0x24,0x10,0x45,0,0,0}, 8,4,1},
    {0x08666e9a, {0x83,0xf8,0x45}, 3,2,1},       /* status/exp rules */
    {0x0866a4a6, {0x83,0xf8,0x45}, 3,2,1},       /* experience gate */
    {0x0866a657, {0x83,0xf8,0x46}, 3,2,0},
    {0x0866a926, {0xc7,0x45,0xe0,0x46,0,0,0}, 7,3,0},
    {0x0866a93f, {0x83,0xf8,0x46}, 3,2,0},
    {0x08689d48, {0x83,0x7d,0xf4,0x45}, 4,3,1}, /* level-up items */
    {0x0868fecb, {0x83,0x7d,0x0c,0x46}, 4,3,0}, /* per-user cap */
    {0x0868fed4, {0xc7,0x80,0xd8,0xeb,0x08,0,0x46,0,0,0}, 10,6,0},
};

static int expected(const NrElf *elf, uintptr_t pc, const void *bytes, size_t size) {
    for (unsigned i = 0; i < elf->header->phnum; ++i) {
        const NrElfProgram *p = &elf->programs[i];
        if (p->type != 1 || pc < p->vaddr || pc - p->vaddr > p->filesz) continue;
        uint64_t offset = p->offset + pc - p->vaddr;
        if (size <= p->filesz - (pc - p->vaddr) && offset <= elf->file_size && size <= elf->file_size - offset)
            return !memcmp(elf->file + offset, bytes, size) && !memcmp((void *)pc, bytes, size);
    }
    return 0;
}

static int patch(uintptr_t pc, const void *bytes, size_t size) {
    DWORD old, ignored;
    if (!VirtualProtect((void *)pc, size, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)pc, bytes, size);
    return VirtualProtect((void *)pc, size, old, &ignored) && FlushInstructionCache(GetCurrentProcess(), (void *)pc, size);
}
#endif

int nr_game_configure(NrRun *run, const char **error) {
    const char *value = NULL;
    const char prefix[] = "DNF_NATIVE_MAX_LEVEL=";
    for (int i = 0; i < run->environment_count; ++i) {
        if (strncmp(run->environment[i], prefix, sizeof(prefix) - 1)) continue;
        if (value) { *error = "duplicate DNF_NATIVE_MAX_LEVEL"; return 0; }
        value = run->environment[i] + sizeof(prefix) - 1;
    }
    NrElf *elf = &run->program;
    int supported = !strcmp(elf->sha256, "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979") && !elf->bias;
    if (!supported) {
        if (value) { *error = "level selection requires the pinned original i386 game ELF"; return 0; }
        return 1;
    }
#ifdef _WIN64
    *error = "game rules require the i386 host";
    return 0;
#else
    unsigned limit = 70;
    if (value) {
        size_t length = strlen(value);
        if (length < 2 || length > 3 || strspn(value, "0123456789") != length || !game_level_valid((unsigned)atoi(value))) {
            *error = "unsupported maximum level"; return 0;
        }
        limit = (unsigned)atoi(value);
    }
    /* get_byte returns true on success. The existing JE +0 enters the failure
       block on both outcomes. The success block follows that block's return;
       compute the branch displacement from those original control-flow sites. */
    const uintptr_t branch = 0x081db4ea, success = 0x081db512;
    const unsigned char old_branch[] = {0x74,0x00};
    *error = "game rules original instructions differ";
    if (!expected(elf, branch, old_branch, sizeof(old_branch))) return 0;
    for (unsigned i = 0; i < sizeof(levels) / sizeof(*levels); ++i)
        if (!expected(elf, levels[i].pc, levels[i].bytes, levels[i].length)) return 0;
    *error = "game rules memory patch failed";
    for (unsigned i = 0; i < sizeof(levels) / sizeof(*levels); ++i) {
        const LevelSite *s = &levels[i];
        unsigned char code[10]; memcpy(code, s->bytes, s->length);
        code[s->operand] = (unsigned char)(limit - s->previous);
        if (!patch(s->pc, code, s->length)) return 0;
        fprintf(stderr, "game level sha256=%s pc=0x%lx operand=0x%lx original=%02x replacement=%02x scope=execution-memory\n",
                elf->sha256, (unsigned long)s->pc, (unsigned long)(s->pc + s->operand), s->bytes[s->operand], code[s->operand]);
    }
    unsigned char corrected[] = {0x74,(unsigned char)(success - (branch + 2))};
    if (!patch(branch, corrected, sizeof(corrected))) return 0;
    fprintf(stderr, "game growth read sha256=%s pc=0x%lx original=7400 replacement=%02x%02x scope=execution-memory; successful reads continue, failed reads retain original errors\n",
            elf->sha256, (unsigned long)branch, corrected[0], corrected[1]);
    fprintf(stderr, "game maximum level=%u; PVF/client progression remains content-dependent; restart required to change\n", limit);
    *error = NULL;
    return 1;
#endif
}

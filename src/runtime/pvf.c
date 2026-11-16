#include "run.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN64
static const char game_hash[] = "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979";
enum { LAYOUT_RESULT = 0x089c1d31, ACTION_PATHS = 0x089c1e80, LAYOUT_RETURN = 0x089c2826,
       CREATURE_PARSED = 0x0833ccdf, SYNC_INSTANCE = 0x0861cbb4,
       STRING_ASSIGN = 0x08708720, EMPTY_STRING = 0x08e15e80 };

/* Original server metadata uses explicit layers/variation names, not the .ani
   paths. A PVF may omit an action-path layout. Keep original
   layer commit, clear, explicit layer assignment and variation-name assignment;
   omit only action-path expansion when a template is absent and the equipment
   supplies its layer. The decision does not depend on a PVF or template name.
   Never substitute another character's layout or invent a default layer. */
static int original_bytes(const NrElf *elf, uintptr_t pc, const unsigned char *bytes, size_t length) {
    for (unsigned i = 0; i < elf->header->phnum; ++i) {
        const NrElfProgram *s = &elf->programs[i];
        if (s->type != 1 || pc < s->vaddr) continue;
        size_t offset = pc - s->vaddr;
        if (offset > s->filesz || length > s->filesz - offset ||
            s->offset > elf->file_size || offset > elf->file_size - s->offset ||
            length > elf->file_size - s->offset - offset) continue;
        return !memcmp(elf->file + s->offset + offset, bytes, length) &&
               !memcmp((const void *)pc, bytes, length);
    }
    return 0;
}

static int layout_failure(const char *reason) {
    fprintf(stderr, "pvf script compatibility failed: %s\n", reason);
    return -1;
}
#endif

int nr_pvf_encoding(NrRun *run, const char **error) {
    const char *encoding = NULL;
    const char prefix[] = "DNF_NATIVE_SCRIPT_ENCODING=";
    for (int i = 0; i < run->environment_count; ++i) {
        if (strncmp(run->environment[i], prefix, sizeof(prefix) - 1)) continue;
        if (encoding) { *error = "duplicate DNF_NATIVE_SCRIPT_ENCODING"; return 0; }
        encoding = run->environment[i] + sizeof(prefix) - 1;
    }
    if (!encoding) return 1;
#ifdef _WIN64
    *error = "script encoding selection requires the supported i386 game ELF";
    return 0;
#else
    NrElf *elf = &run->program;
    if (strcmp(elf->sha256, game_hash) || elf->bias || (strcmp(encoding, "GBK") && strcmp(encoding, "BIG5"))) {
        *error = "script encoding selection supports GBK/BIG5 on the fixed game ELF only";
        return 0;
    }
    const uintptr_t pc = 0x08108da2;
    static const unsigned char original[] = {0xc7, 0x44, 0x24, 0x04, 0xd5, 0x46, 0xb4, 0x08};
    if (!original_bytes(elf, pc, original, sizeof(original)) ||
        !original_bytes(elf, 0x08b446d1, (const unsigned char *)"GBK\0BIG5", 9)) {
        *error = "script encoding selection original bytes/constants differ";
        return 0;
    }
    if (!strcmp(encoding, "GBK")) {
        DWORD old, ignored;
        uint32_t value = 0x08b446d1;
        if (!VirtualProtect((void *)pc, sizeof(original), PAGE_EXECUTE_READWRITE, &old)) return 0;
        memcpy((void *)(pc + 4), &value, sizeof(value));
        if (!VirtualProtect((void *)pc, sizeof(original), old, &ignored) ||
            !FlushInstructionCache(GetCurrentProcess(), (void *)pc, sizeof(original))) return 0;
        fprintf(stderr, "pvf script encoding sha256=%s encoding=GBK pc=0x%lx original=c7442404d546b408 replacement=c7442404d146b408 scope=execution-memory\n",
                elf->sha256, (unsigned long)pc);
    } else {
        fprintf(stderr, "pvf script encoding encoding=BIG5 original behavior retained\n");
    }
    return 1;
#endif
}

int nr_pvf_adapt(NrElf *elf, const char **error) {
#ifdef _WIN64
    (void)elf; (void)error;
#else
    if (strcmp(elf->sha256, game_hash)) return 1;
    static const unsigned char result[] = {0x89, 0x45, 0xe0}; /* mov [ebp-20],eax */
    static const unsigned char actions[] = {0x8b, 0x45, 0x18}; /* mov eax,[ebp+18] */
    static const unsigned char creature[] = {0xe8, 0xd0, 0xfe, 0x2d, 0x00}; /* call GetInstanceSyncScript */
    static const unsigned char finish[] = {0x8d, 0x65, 0xf4, 0x83, 0xc4, 0x00, 0x5b, 0x5e, 0x5f, 0x5d, 0xc3};
    static const unsigned char assign[] = {0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14};
    if (elf->bias || !original_bytes(elf, LAYOUT_RESULT, result, sizeof(result)) ||
        !original_bytes(elf, ACTION_PATHS, actions, sizeof(actions)) ||
        !original_bytes(elf, CREATURE_PARSED, creature, sizeof(creature)) ||
        !original_bytes(elf, LAYOUT_RETURN, finish, sizeof(finish)) ||
        !original_bytes(elf, STRING_ASSIGN, assign, sizeof(assign)) ||
        !original_bytes(elf, EMPTY_STRING, (const unsigned char *)"", 1)) {
        *error = "PVF equipment correction original addresses/bytes differ";
        return 0;
    }
    uintptr_t sites[] = {LAYOUT_RESULT, ACTION_PATHS, CREATURE_PARSED};
    const unsigned char *bytes[] = {result, actions, creature};
    unsigned lengths[] = {sizeof(result), sizeof(actions), sizeof(creature)};
    for (unsigned i = 0; i < 3; ++i) {
        DWORD old, ignored;
        if (!VirtualProtect((void *)sites[i], 2, PAGE_EXECUTE_READWRITE, &old)) return 0;
        memcpy((void *)sites[i], "\x0f\x0b", 2);
        if (!VirtualProtect((void *)sites[i], 2, old, &ignored)) return 0;
        fprintf(stderr, "pvf script correction sha256=%s pc=0x%lx original=", elf->sha256, (unsigned long)sites[i]);
        for (unsigned j = 0; j < lengths[i]; ++j) fprintf(stderr, "%02x", bytes[i][j]);
        fputs(" replacement=0f0b scope=execution-memory\n", stderr);
    }
#endif
    return 1;
}

/* 0 = unrelated site, 1 = handled, -1 = unsupported/malformed equipment. */
int nr_pvf_script(NrRun *run, CONTEXT *c) {
#ifdef _WIN64
    (void)run; (void)c;
#else
    if ((c->Eip != LAYOUT_RESULT && c->Eip != ACTION_PATHS && c->Eip != CREATURE_PARSED) ||
        strcmp(run->program.sha256, game_hash)) return 0;
    if (c->Eip == CREATURE_PARSED) {
        /* importCreatureScript reverse-searches by filename, which collapses
           distinct list IDs sharing a file to the first ID. The caller already
           owns the authoritative current (ID,path) pair. Use that ID before
           Set(), item binding, DB serialization and duplicate detection. */
        if (c->Ebp < 0x6b8 || !nr_memory(c->Ebp - 0x6b8, 0x69c, 1) ||
            c->Esp < 4 || !nr_memory(c->Esp - 4, 4, 1)) return layout_failure("invalid creature loader frame");
        uintptr_t node = *(uint32_t *)(c->Ebp - 0x64);
        if (!nr_memory(node, 24, 0)) return layout_failure("invalid creature list iterator");
        int id = *(int *)(node + 16), parsed = *(int *)(c->Ebp - 0x6b8);
        uintptr_t path = *(uint32_t *)(node + 20);
        if (id <= 0 || path != *(uint32_t *)(c->Ebp - 0x20))
            return layout_failure("creature list ID/path mismatch");
        if (id != parsed) {
            fprintf(stderr, "pvf creature shared script list_id=%d reverse_lookup_id=%d; preserving authoritative list ID\n", id, parsed);
            *(int *)(c->Ebp - 0x6b8) = id;
        }
        c->Esp -= 4;
        *(uint32_t *)c->Esp = CREATURE_PARSED + 5;
        c->Eip = SYNC_INSTANCE; /* Original CALL, with unchanged flags. */
        return 1;
    }
    if (c->Ebp < 0x44 || !nr_memory(c->Ebp - 0x44, 0x6c, 1))
        return layout_failure("invalid original layout frame");
    if (c->Eip == LAYOUT_RESULT) {
        uintptr_t map = *(uint32_t *)(c->Ebp + 8);
        uintptr_t iterator = *(uint32_t *)(c->Ebp - 0x44);
        if (iterator == map + 4) { /* Verified libstdc++ tree header/end layout. */
            uintptr_t key_object = *(uint32_t *)(c->Ebp + 0x10);
            if (!nr_memory(key_object, 4, 0)) return layout_failure("invalid layout key");
            uintptr_t key = *(uint32_t *)key_object;
            /* This ELF uses the COW string ABI: length is 12 bytes before data.
               Bound diagnostic output without imposing a template-name limit. */
            if (key < 12 || !nr_memory(key - 12, 4, 0)) return layout_failure("invalid layout string");
            uint32_t length = *(uint32_t *)(key - 12);
            if (length == UINT32_MAX || !nr_memory(key, length + 1, 0) || *(char *)(key + length))
                return layout_failure("invalid layout string extent");
            int layer = *(int *)(c->Ebp + 0x14);
            unsigned job = *(uint32_t *)(c->Ebp + 0x20), type = *(uint32_t *)(c->Ebp + 0x24);
            fprintf(stderr, "pvf visual layout absent key=%.*s layer=%d job=%u type=%u\n",
                    (int)(length < 256 ? length : 256), (const char *)key, layer, job, type);
            if (layer <= 0 || job > 11 || type > 2)
                return layout_failure("missing layout required to supply layer or unsupported animation mode");
            c->Eax = 0; /* Local marker; never dereference end()->second. */
        }
        *(uint32_t *)(c->Ebp - 0x20) = c->Eax;
        c->Eip += 3; /* Original MOV has no flag effects. */
    } else if (!*(uint32_t *)(c->Ebp - 0x20)) {
        /* Original clear omits the final action string at +5c. Clear that too
           with the original string assignment (including COW ownership), so a
           previous valid layer cannot leak its path into this missing layout.
           Execute a guest CALL using its existing outgoing argument area, then
           return through the original saved-register epilogue. Layer and
           variation name have already been assigned; no live temporary remains. */
        uintptr_t ani = *(uint32_t *)(c->Ebp + 0x0c);
        if (!nr_memory(ani, 0x7c, 1) || c->Esp < 4 || !nr_memory(c->Esp - 4, 12, 1))
            return layout_failure("invalid layout cleanup frame");
        c->Esp -= 4;
        uint32_t *args = (uint32_t *)c->Esp;
        args[0] = LAYOUT_RETURN;
        args[1] = ani + 0x5c;
        args[2] = EMPTY_STRING;
        c->Eip = STRING_ASSIGN;
    } else {
        c->Eax = *(uint32_t *)(c->Ebp + 0x18);
        c->Eip += 3;
    }
    return 1;
#endif
    return 0;
}

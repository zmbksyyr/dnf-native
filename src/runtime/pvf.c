#include "run.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN64
static const char game_hash[] = "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979";
enum { LAYOUT_RESULT = 0x089c1d31, ACTION_PATHS = 0x089c1e80, LAYOUT_RETURN = 0x089c2826,
       CREATURE_PARSED = 0x0833ccdf, SYNC_INSTANCE = 0x0861cbb4,
       STRING_TABLE = 0x088bb551, SCRIPT_CODEPAGE = 0x093febf8,
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

/* The original PackSet decrypts the table. Inspect only its string spans, not
   encrypted bytes, filenames or a PVF identity. Offsets are relative to +4.
   Strict decoding and ordinary Chinese function words distinguish GBK/Big5;
   neither "no conversion error" nor a single readable string proves a charset. */
static unsigned table_encoding(const unsigned char *table, uint32_t size) {
    if (size < 8) return 0;
    uint32_t count;
    memcpy(&count, table, 4);
    if (count > (size - 8) / 4) return 0;
    const uint32_t *offsets = (const uint32_t *)(table + 4);
    uint32_t begin = offsets[0];
    if (begin < (count + 1) * 4 || begin > size - 4) return 0;
    for (uint32_t i = 1; i <= count; ++i) {
        if (offsets[i] < offsets[i - 1] || offsets[i] > size - 4) return 0;
    }
    const UINT pages[] = {936, 950};
    /* A small, independently chosen language sample; no reference byte tables. */
    static const wchar_t common[] = L"的一是不在人有我和这這为為个個中来來上大们們到说說时時要会會对對能以地得可也就出子生自年过過于於着著下";
    int64_t scores[2] = {0};
    unsigned invalid[2] = {0}, words[2] = {0}, private_chars[2] = {0}, samples = 0;
    wchar_t *wide = NULL;
    size_t capacity = 0;
    uint32_t stride = count > 16384 ? count / 16384 : 1;
    for (uint32_t i = 0; i < count; i += stride) {
        uint32_t length = offsets[i + 1] - offsets[i];
        const char *text = (const char *)table + 4 + offsets[i];
        unsigned nonascii = 0;
        for (uint32_t j = 0; j < length; ++j) nonascii += (unsigned char)text[j] >= 128;
        if (!nonascii) continue;
        if (length > INT_MAX) { free(wide); return 0; }
        if (capacity < length) {
            wchar_t *larger = realloc(wide, (size_t)length * sizeof(*wide));
            if (!larger) { free(wide); return 0; }
            wide = larger;
            capacity = length;
        }
        ++samples;
        for (unsigned p = 0; p < 2; ++p) {
            int n = MultiByteToWideChar(pages[p], MB_ERR_INVALID_CHARS, text, (int)length, wide, (int)capacity);
            if (!n) { invalid[p] += nonascii; scores[p] -= nonascii; continue; }
            for (int j = 0; j < n; ++j) {
                if (wide[j] && wcschr(common, wide[j])) { ++words[p]; scores[p] += 8; }
                if (wide[j] >= 0xe000 && wide[j] <= 0xf8ff) { ++private_chars[p]; scores[p] -= 4; }
            }
        }
    }
    free(wide);
    unsigned chosen = 0;
    if (!samples) chosen = 950; /* ASCII is identical in both encodings. */
    else if (scores[0] > 0 && scores[0] - scores[1] >= 16) chosen = 936;
    else if (scores[1] > 0 && scores[1] - scores[0] >= 16) chosen = 950;
    fprintf(stderr, "pvf script encoding auto samples=%u GBK(score=%lld invalid_bytes=%u common=%u private=%u) BIG5(score=%lld invalid_bytes=%u common=%u private=%u) selected=%s\n",
            samples, (long long)scores[0], invalid[0], words[0], private_chars[0],
            (long long)scores[1], invalid[1], words[1], private_chars[1],
            chosen == 936 ? "GBK" : chosen == 950 ? "BIG5" : "ambiguous");
    return chosen;
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
    run->script_encoding = !strcmp(encoding, "GBK") ? 936 : 950;
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
    static const unsigned char table[] = {0x8b, 0x45, 0xf0}; /* mov eax,[ebp-10] */
    if (elf->bias || !original_bytes(elf, LAYOUT_RESULT, result, sizeof(result)) ||
        !original_bytes(elf, ACTION_PATHS, actions, sizeof(actions)) ||
        !original_bytes(elf, CREATURE_PARSED, creature, sizeof(creature)) ||
        !original_bytes(elf, LAYOUT_RETURN, finish, sizeof(finish)) ||
        !original_bytes(elf, STRING_TABLE, table, sizeof(table)) ||
        !original_bytes(elf, 0x08b446d1, (const unsigned char *)"GBK\0BIG5", 9) ||
        !original_bytes(elf, STRING_ASSIGN, assign, sizeof(assign)) ||
        !original_bytes(elf, EMPTY_STRING, (const unsigned char *)"", 1)) {
        *error = "PVF equipment correction original addresses/bytes differ";
        return 0;
    }
    uintptr_t sites[] = {LAYOUT_RESULT, ACTION_PATHS, CREATURE_PARSED, STRING_TABLE};
    const unsigned char *bytes[] = {result, actions, creature, table};
    unsigned lengths[] = {sizeof(result), sizeof(actions), sizeof(creature), sizeof(table)};
    for (unsigned i = 0; i < 4; ++i) {
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
    if ((c->Eip != LAYOUT_RESULT && c->Eip != ACTION_PATHS && c->Eip != CREATURE_PARSED && c->Eip != STRING_TABLE) ||
        strcmp(run->program.sha256, game_hash)) return 0;
    if (c->Eip == STRING_TABLE) {
        if (c->Ebp < 0x10 || !nr_memory(c->Ebp - 0x10, 8, 0))
            return layout_failure("invalid string table frame");
        uint32_t size = *(uint32_t *)(c->Ebp - 0x10);
        if (run->script_encoding_assigning) {
            run->script_encoding_assigning = 0;
            c->Eax = size;
            c->Eip += 3; /* Original MOV, flags unchanged. */
            return 1;
        }
        uintptr_t table = *(uint32_t *)(c->Ebp - 0x0c);
        if (!nr_memory(table, size, 0)) return layout_failure("invalid string table extent");
        unsigned encoding = run->script_encoding ? run->script_encoding : table_encoding((const unsigned char *)table, size);
        if (!encoding) return layout_failure("PVF encoding ambiguous or malformed; DNF_NATIVE_SCRIPT_ENCODING can explicitly select GBK/BIG5");
        if (!nr_memory(SCRIPT_CODEPAGE, 4, 1) || c->Esp < 4 || !nr_memory(c->Esp - 4, 12, 1))
            return layout_failure("invalid script codepage assignment frame");
        fprintf(stderr, "pvf script encoding source=%s encoding=%s; original string assignment and conversion retained\n",
                run->script_encoding ? "explicit" : "auto", encoding == 936 ? "GBK" : "BIG5");
        run->script_encoding_assigning = 1;
        c->Esp -= 4;
        uint32_t *args = (uint32_t *)c->Esp;
        args[0] = STRING_TABLE;
        args[1] = SCRIPT_CODEPAGE;
        args[2] = encoding == 936 ? 0x08b446d1 : 0x08b446d5;
        c->Eip = STRING_ASSIGN;
        return 1;
    }
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

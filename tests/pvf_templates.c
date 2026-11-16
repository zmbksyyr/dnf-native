/* Original ELF function tests, after its real loader and global constructors.
   Only the fixture main is replaced; product compatibility handlers are intact. */
#include "run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t name, value, size; unsigned char info, other; uint16_t section; } Symbol;
static NrRun *fixture_run;
static uintptr_t main_pc;
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "PVF_TEMPLATE failed line=%d\n", __LINE__); ExitProcess(112); } } while (0)

static uintptr_t symbol(const char *name) {
    NrElf *e = &fixture_run->program;
    NrElfSection *sections = (NrElfSection *)(e->file + e->header->shoff);
    for (unsigned i = 0; i < e->header->shnum; ++i) if (sections[i].type == 2) {
        const Symbol *symbols = (const Symbol *)(e->file + sections[i].offset);
        const char *names = (const char *)(e->file + sections[sections[i].link].offset);
        for (unsigned j = 0; j < sections[i].size / sizeof(*symbols); ++j)
            if (!strcmp(names + symbols[j].name, name)) return e->bias + symbols[j].value;
    }
    fprintf(stderr, "PVF_TEMPLATE missing symbol=%s\n", name);
    ExitProcess(112);
}

static void string_init(uint32_t *value, const char *text) {
    unsigned char allocator = 0;
    ((void (*)(void *, const char *, void *))symbol("_ZNSsC1EPKcRKSaIcE"))(value, text, &allocator);
}

static void string_set(uint32_t *value, const char *text) {
    ((void (*)(void *, const char *))0x08708720)(value, text);
}

static void string_free(uint32_t *value) {
    ((void (*)(void *))symbol("_ZNSsD1Ev"))(value);
}

static unsigned actions(const unsigned char *entry) {
    unsigned count = 0;
    for (unsigned offset = 4; offset < 0x60; offset += 4)
        if (**(char *const *)(entry + offset)) ++count;
    return count;
}

static void check_entry(const unsigned char *entry, int layer, const char *path, int visual) {
    CHECK(*(const int *)entry == layer);
    CHECK(!strcmp(*(char *const *)(entry + 0x60), path));
    CHECK((actions(entry) != 0) == visual);
    if (!visual) {
        CHECK(*(const uint32_t *)(entry + 0x64) == *(const uint32_t *)(entry + 0x68));
        CHECK(*(const uint32_t *)(entry + 0x70) == *(const uint32_t *)(entry + 0x74));
    }
}

static void set_layout(void *map, void *ani, uint32_t *key, int layer, uint32_t *path,
                       void *equipment, unsigned job, unsigned type) {
    ((void (*)(void *, void *, void *, int, void *, void *, unsigned, unsigned))0x089c1cb9)
        (map, ani, key, layer, path, equipment, job, type);
}

static const unsigned char *find_template(void *map, const char *key) {
    /* Read the original libstdc++ tree, independently of the adapted setter. */
    uintptr_t node = *(uint32_t *)((unsigned char *)map + 8);
    while (node) {
        int comparison = strcmp(key, *(char **)(node + 16));
        if (!comparison) return (const unsigned char *)(node + 20);
        node = *(uint32_t *)(node + (comparison < 0 ? 8 : 12));
    }
    CHECK(0);
    return NULL;
}

static void layout_case(void *map, unsigned job, unsigned type) {
    unsigned char equipment[4096], ani[0x7c];
    uint32_t key, path;
    ((void (*)(void *))symbol("_ZN17STEquipmentScriptC1Ev"))(equipment);
    ((void (*)(void *))symbol("_ZN20STEquipmentAniScriptC1Ev"))(ani);
    string_init(&key, "equipment/character/swordman.lay");
    string_init(&path, "original_layer");
    unsigned char *info = ((void *(*)(void *, unsigned))0x0898efa8)(equipment, job);
    *(int *)(info + 0x0c) = 12; *(int *)(info + 0x10) = 3;
    *(int *)(info + 0x30) = 68; *(int *)(info + 0x34) = 1;
    if (type == 2) {
        uint32_t empty_group[3] = {0};
        ((void (*)(void *, void *))symbol("_ZNSt6vectorIS_I20STEquipmentAniScriptSaIS0_EESaIS2_EE9push_backEOS2_"))
            (info + 0x5c, empty_group);
    }
    set_layout(map, ani, &key, 101, &path, equipment, job, type);
    check_entry(ani, 101, "original_layer", 1);
    /* Unrelated names and case: no known PVF or character-name allowlist. */
    string_set(&key, "equipment/character/future_class.lay");
    string_set(&path, "future_a");
    set_layout(map, ani, &key, 202, &path, equipment, job, type);
    check_entry(ani, 202, "future_a", 0);
    string_set(&key, "Custom/Unrelated/SecondLayout.lay");
    string_set(&path, "future_b");
    set_layout(map, ani, &key, 303, &path, equipment, job, type);
    check_entry(ani, 303, "future_b", 0);
    string_set(&key, "equipment/character/swordman.lay");
    string_set(&path, "restored_layer");
    set_layout(map, ani, &key, 404, &path, equipment, job, type);
    check_entry(ani, 404, "restored_layer", 1);

    const uint32_t *vector = (const uint32_t *)(info + (type == 0 ? 0 : type == 1 ? 0x18 : 0x5c));
    if (type == 2) { CHECK(vector[1] - vector[0] == 12); vector = (const uint32_t *)(uintptr_t)vector[0]; }
    CHECK(vector[1] - vector[0] == 3 * 0x7c);
    const unsigned char *entries = (const unsigned char *)(uintptr_t)vector[0];
    check_entry(entries, 101, "original_layer", 1);
    check_entry(entries + 0x7c, 202, "future_a", 0);
    check_entry(entries + 2 * 0x7c, 303, "future_b", 0);
    CHECK(*(int *)(info + 0x0c) == 12 && *(int *)(info + 0x10) == 3);
    CHECK(*(int *)(info + 0x30) == 68 && *(int *)(info + 0x34) == 1);
    string_free(&key); string_free(&path);
    ((void (*)(void *))symbol("_ZN20STEquipmentAniScriptD1Ev"))(ani);
    ((void (*)(void *))symbol("_ZN17STEquipmentScriptD1Ev"))(equipment);
    printf("PVF_TEMPLATE mixed job=%u type=%u layers=3 metadata=preserved\n", job, type);
}

static int fixture_main(int argc, char **argv, char **environment) {
    (void)environment;
    CHECK(argc == 3);
    ((void (*)(void))symbol("_ZN8CodePage12initCodePageEv"))();
    if (!strcmp(argv[2], "encoding-synthetic")) {
        const UINT pages[] = {936, 950, 0, 0, 0};
        for (unsigned i = 0; i < 5; ++i) {
            uint32_t table[128] = {1, 8, 8};
            const wchar_t *text = i == 0 ? L"\u8fd9\u662f\u4e00\u4e2a\u4eba\u7684" : L"\u9019\u662f\u4e00\u500b\u4eba\u7684";
            int length = i < 2 ? WideCharToMultiByte(pages[i], WC_NO_BEST_FIT_CHARS, text, -1,
                                                    (char *)(table + 3), sizeof(table) - 12, NULL, NULL) : 1;
            CHECK(length > 0);
            table[2] += (unsigned)length;
            if (i == 3) { table[3] = 0xa1; } /* Incomplete multibyte text. */
            if (i == 4) { ++table[2]; } /* Offset past the actual extent. */
            uint32_t frame[8] = {(uint32_t)(12 + length), (uint32_t)(uintptr_t)table}, outgoing[4];
            CONTEXT context = {0};
            context.Eip = 0x088bb551;
            context.Ebp = (DWORD)(uintptr_t)(frame + 4);
            context.Esp = (DWORD)(uintptr_t)(outgoing + 1);
            int result = nr_pvf_script(fixture_run, &context);
            if (i >= 3) { CHECK(result == -1); continue; }
            CHECK(result == 1 && context.Eip == 0x08708720);
            ((void (*)(void *, const char *))0x08708720)((void *)(uintptr_t)outgoing[1], (const char *)(uintptr_t)outgoing[2]);
            const char *selected = ((const char *(*)(void))symbol("_ZN8CodePage6scriptEv"))();
            CHECK(!strcmp(selected, i == 0 ? "GBK" : "BIG5"));
            context.Eip = 0x088bb551;
            CHECK(nr_pvf_script(fixture_run, &context) == 1 && context.Eip == 0x088bb554 && context.Eax == frame[0]);
        }
        puts("PVF_ENCODING synthetic GBK/BIG5/ASCII selected; ambiguous/malformed rejected");
        return 42;
    }
    int error = -1;
    CHECK(((int (*)(const char *, int, const char *, int *))symbol("_Z14InitRDARScriptPKcbS0_Pi"))
          (".", 1, argv[1], &error) && !error);
    if (!strncmp(argv[2], "encoding-", 9)) {
        const char *encoding = ((const char *(*)(void))symbol("_ZN8CodePage6scriptEv"))();
        CHECK(!strcmp(encoding, argv[2] + 9));
        printf("PVF_ENCODING original-loader script=%s\n", encoding);
        return 42;
    }
    ((void (*)(void))symbol("_Z25InitMapEquipmentAniScriptv"))();
    void *map = *(void **)symbol("g_mapEquipmentAniScript");
    if (!strcmp(argv[2], "mixed")) {
        for (unsigned job = 0; job <= 11; ++job)
            for (unsigned type = 0; type <= 2; ++type) layout_case(map, job, type);
        return 42;
    }
    unsigned char equipment[4096], ani[0x7c]; uint32_t key, path;
    ((void (*)(void *))symbol("_ZN17STEquipmentScriptC1Ev"))(equipment);
    ((void (*)(void *))symbol("_ZN20STEquipmentAniScriptC1Ev"))(ani);
    int present = !strcmp(argv[2], "present-default");
    string_init(&key, present ? "equipment/character/swordman.lay" : "equipment/unknown/missing.lay");
    string_init(&path, "test_layer");
    int layer = !strcmp(argv[2], "negative-layer") ? -1 : 0;
    unsigned job = !strcmp(argv[2], "invalid-job") ? 12 : 11;
    unsigned type = !strcmp(argv[2], "invalid-type") ? 3 : 0;
    if (job == 12 || type == 3) layer = 1;
    set_layout(map, ani, &key, layer, &path, equipment, job, type);
    CHECK(present); /* Missing required layer and unsupported modes must fail. */
    const unsigned char *definition = find_template(map, "equipment/character/swordman.lay");
    CHECK(*(int *)ani == *(const int *)definition && actions(ani));
    printf("PVF_TEMPLATE present-default layer=%d\n", *(int *)ani);
    string_free(&key); string_free(&path);
    ((void (*)(void *))symbol("_ZN20STEquipmentAniScriptD1Ev"))(ani);
    ((void (*)(void *))symbol("_ZN17STEquipmentScriptD1Ev"))(equipment);
    return 42;
}

void nr_probe_install(NrRun *run) {
    fixture_run = run;
    CHECK(!strcmp(run->program.sha256, "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979"));
    main_pc = symbol("main");
    CHECK(nr_memory(main_pc, 3, 0) && !memcmp((void *)main_pc, "\x55\x89\xe5", 3));
    DWORD old, ignored;
    CHECK(VirtualProtect((void *)main_pc, 2, PAGE_EXECUTE_READWRITE, &old));
    memcpy((void *)main_pc, "\x0f\x0b", 2);
    CHECK(VirtualProtect((void *)main_pc, 2, old, &ignored));
    CHECK(FlushInstructionCache(GetCurrentProcess(), (void *)main_pc, 2));
    fprintf(stderr, "PVF_TEMPLATE fixture main pc=0x%lx original=5589e5 replacement=0f0b scope=execution-memory\n", (unsigned long)main_pc);
}

int nr_probe_trap(EXCEPTION_POINTERS *p) {
    if (p->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION || p->ContextRecord->Eip != main_pc) return 0;
    p->ContextRecord->Eip = (DWORD)(uintptr_t)fixture_main;
    return 1;
}

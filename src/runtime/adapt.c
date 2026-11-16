#include "elf.h"
#include <Zydis/Decoder.h>
#include <Zydis/Encoder.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN64
DWORD nr_thread_tls_slot(void);
void nr_tls_uninitialized(void);

/* Absolute GS reads dominate glibc's thread/canary checks. Read our per-thread
   pointer from the SDK TEB layout, then execute the same instruction natively.
   Other TLS operands keep the existing exception path. */
static int direct_tls(NrElf *elf, uintptr_t pc, const ZydisDecodedInstruction *instruction,
                      const ZydisDecodedOperand *operands) {
    DWORD slot = nr_thread_tls_slot();
    if (slot >= TLS_MINIMUM_AVAILABLE || instruction->length < 5 || instruction->address_width != 32 ||
        instruction->operand_count_visible != 2 ||
        (instruction->mnemonic != ZYDIS_MNEMONIC_MOV && instruction->mnemonic != ZYDIS_MNEMONIC_XOR &&
         instruction->mnemonic != ZYDIS_MNEMONIC_CMP)) return 0;
    unsigned memory = operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? 0 : 1;
    const ZydisDecodedOperand *operand = &operands[memory];
    if (operand->type != ZYDIS_OPERAND_TYPE_MEMORY || operand->mem.segment != ZYDIS_REGISTER_GS ||
        operand->mem.base != ZYDIS_REGISTER_NONE || operand->mem.index != ZYDIS_REGISTER_NONE ||
        (operand->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) || operand->size != 32) return 0;
    unsigned scratch = 1; /* ECX, or EDX if the original instruction uses ECX. */
    for (unsigned i = 0; i < 2; ++i) {
        if (operands[i].type != ZYDIS_OPERAND_TYPE_REGISTER) continue;
        if (operands[i].reg.value == ZYDIS_REGISTER_ESP || operands[i].size != 32) return 0;
        if (operands[i].reg.value == ZYDIS_REGISTER_ECX) scratch = 2;
    }
    ZydisEncoderRequest request;
    if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(instruction, operands, 2, &request))) return 0;
    request.prefixes &= ~ZYDIS_ATTRIB_HAS_SEGMENT;
    request.operands[memory].mem.base = scratch == 1 ? ZYDIS_REGISTER_ECX : ZYDIS_REGISTER_EDX;
    unsigned char code[64];
    code[0] = (unsigned char)(0x50 + scratch); /* PUSH scratch preserves flags. */
    code[1] = 0x9c;
    code[2] = 0x64; code[3] = 0x8b; code[4] = (unsigned char)(5 | (scratch << 3));
    uint32_t tls_offset = (uint32_t)offsetof(TEB, TlsSlots) + slot * 4;
    memcpy(code + 5, &tls_offset, 4);
    code[9] = 0x8b; code[10] = (unsigned char)((scratch << 3) | scratch); /* NrThread.tls_base is first. */
    code[11] = 0x85; code[12] = (unsigned char)(0xc0 | (scratch << 3) | scratch);
    code[13] = 0x74; /* Uninitialized TLS retains the existing explicit failure. */
    code[15] = 0x9d;
    ZyanUSize length = 15;
    if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&request, code + 16, &length))) return 0;
    size_t bytes = 16 + length;
    code[bytes++] = (unsigned char)(0x58 + scratch);
    code[bytes++] = 0xe9;
    NrCodePage *page = elf->tls_pages;
    if (!page || sizeof(page->code) - page->used < bytes + 11) {
        page = VirtualAlloc(NULL, sizeof(*page), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!page) return 0;
        page->next = elf->tls_pages; elf->tls_pages = page;
    }
    unsigned char *target = page->code + page->used;
    uint32_t back = (uint32_t)(pc + instruction->length - (uintptr_t)(target + bytes + 4));
    memcpy(code + bytes, &back, 4); bytes += 4;
    code[14] = (unsigned char)(bytes - 15);
    code[bytes++] = 0x9d; code[bytes++] = (unsigned char)(0x58 + scratch); code[bytes++] = 0xe9;
    uint32_t failure = (uint32_t)((uintptr_t)nr_tls_uninitialized - (uintptr_t)(target + bytes + 4));
    memcpy(code + bytes, &failure, 4); bytes += 4;
    memcpy(target, code, bytes); page->used += bytes;
    DWORD old, ignored;
    if (!VirtualProtect((void *)pc, instruction->length, PAGE_EXECUTE_READWRITE, &old)) return 0;
    unsigned char jump[15]; memset(jump, 0x90, instruction->length); jump[0] = 0xe9;
    uint32_t displacement = (uint32_t)((uintptr_t)target - (pc + 5));
    memcpy(jump + 1, &displacement, 4);
    memcpy((void *)pc, jump, instruction->length);
    if (!VirtualProtect((void *)pc, instruction->length, old, &ignored)) return -1;
    return 1;
}
#endif

static uintptr_t got_address(const NrElf *elf) {
    for (unsigned i = 0; i < elf->header->phnum; ++i) {
        const NrElfProgram *p = &elf->programs[i];
        if (p->type != 2 || p->offset > elf->file_size || p->filesz > elf->file_size - p->offset) continue;
        for (uint64_t off = 0; off + 2 * sizeof(uintptr_t) <= p->filesz; off += 2 * sizeof(uintptr_t)) {
            uintptr_t pair[2];
            memcpy(pair, elf->file + p->offset + off, sizeof(pair));
            if (pair[0] == 3) return pair[1];
            if (!pair[0]) break;
        }
    }
    return 0;
}

static int trap_compare(const void *left, const void *right) {
    const NrTrap *a = left, *b = right;
    return a->pc > b->pc ? 1 : a->pc < b->pc ? -1 : 0;
}

static size_t frida_inline_data_end(const NrElf *elf, const NrElfSection *section, size_t offset) {
    /* This exact Frida build embeds AES tables in .text before the next function. */
    if (strcmp(elf->sha256, "8ca458e422dead8081c6e25831050dcbb006794c811bc2fe471f0cb59fdcc239") == 0 &&
        section->addr == 0x7c000 && section->offset == 0x7c000) {
        if (offset >= 0xc286c0 && offset < 0xc29300) return 0xc29300;
        if (offset >= 0xc29d40 && offset < 0xc2a940) return 0xc2a940;
        if (offset >= 0xc2e5c0 && offset < 0xc2e9c0) return 0xc2e9c0;
        if (offset >= 0xc32280 && offset < 0xc332c0) return 0xc332c0;
        if (offset >= 0xc344c0 && offset < 0xc34580) return 0xc34580;
        if (offset >= 0xc3a700 && offset < 0xc3a7c0) return 0xc3a7c0;
        if (offset >= 0xc3aac0 && offset < 0xc3ac20) return 0xc3ac20;
        if (offset >= 0xc42300 && offset < 0xc425e0) return 0xc425e0;
        if (offset >= 0xcd0000 && offset < 0xcf509c) return 0xcf509c;
        if (offset >= 0xcf8700 && offset < 0xcf89f1) return 0xcf89f1;
        if (offset >= 0xcfa923 && offset < 0xcfaa00) return 0xcfaa00;
        if (offset >= 0xd0eac0 && offset < 0xd0fb10) return 0xd0fb10;
    }
    return 0;
}

static size_t frida_padlock_instruction_end(const NrElf *elf, const NrElfSection *section, size_t offset) {
    /* Zydis 4.1.1 omits the PadLock XSHA256 /r=e0 form used by this build. */
    if (strcmp(elf->sha256, "8ca458e422dead8081c6e25831050dcbb006794c811bc2fe471f0cb59fdcc239") == 0 &&
        section->addr == 0x7c000 && section->offset == 0x7c000 && offset == 0xc4307a &&
        memcmp(elf->file + section->offset + offset, "\xf3\x0f\xa6\xe0", 4) == 0)
        return offset + 4;
    return 0;
}

int nr_adapt(NrElf *elf, const char **error) {
    NrElfHeader *h = elf->header;
    *error = "execution adaptation currently requires valid executable sections";
    int packed_image = 0;
    for (unsigned i = 0; i < h->phnum; ++i)
        if (elf->programs[i].type == 1 && (elf->programs[i].flags & 7) == 7) packed_image = 1;
    if (!h->shnum) {
        if (!packed_image || !FlushInstructionCache(GetCurrentProcess(), elf->allocation, elf->allocation_size)) return 0;
        fprintf(stderr, "adapt skipped sectionless packed image sha256=%s\n", elf->sha256);
        fprintf(stderr, "adapt sha256=%s sections=0 syscall_traps=0 tls_traps=0\n", elf->sha256);
        *error = NULL;
        return 1;
    }
    if (h->shentsize != sizeof(NrElfSection) || h->shoff > elf->file_size ||
        (uint64_t)h->shnum * sizeof(NrElfSection) > elf->file_size - h->shoff) return 0;
    NrElfSection *sections = (NrElfSection *)(elf->file + h->shoff);
    ZydisDecoder decoder;
#ifdef _WIN64
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif
    size_t capacity = 0, tls_count = 0, direct_count = 0, syscall_count = 0, section_count = 0;
    uintptr_t got = got_address(elf);
    for (unsigned s = 0; s < h->shnum; ++s) {
        NrElfSection *section = &sections[s];
        if (section->type != 1 || (section->flags & 6) != 6 || !section->size) continue;
        if (section->offset > elf->file_size || section->size > elf->file_size - section->offset)
            return 0;
        int mapped = 0, packed = 0;
        for (unsigned i = 0; i < h->phnum; ++i) {
            NrElfProgram *p = &elf->programs[i];
            if (p->type == 1 && (p->flags & 1) && section->addr >= p->vaddr &&
                section->addr - p->vaddr <= p->filesz &&
                section->size <= p->filesz - (section->addr - p->vaddr) &&
                section->offset == p->offset + (section->addr - p->vaddr)) {
                mapped = 1;
                packed |= (p->flags & 7) == 7;
            }
        }
        if (!mapped) {
            if (!packed_image) return 0;
            fprintf(stderr, "adapt skipped unmapped executable section sha256=%s section=%u\n", elf->sha256, s);
            continue;
        }
        if (packed) {
            fprintf(stderr, "adapt skipped writable executable section sha256=%s section=%u\n", elf->sha256, s);
            ++section_count;
            continue;
        }
        ++section_count;
        size_t table_start = 0, table_end = 0, after_sub = 0;
        ZydisRegister table_register = ZYDIS_REGISTER_NONE;
        for (size_t offset = 0; offset < section->size;) {
            if (table_end && offset >= table_start && offset < table_end) {
                fprintf(stderr, "confirmed GCC PIC jump table sha256=%s address=0x%llx bytes=%zu\n", elf->sha256,
                        (unsigned long long)(elf->bias + section->addr + table_start), table_end - table_start);
                offset = table_end;
                table_end = 0;
                if (offset == section->size) break;
            }
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, elf->file + section->offset + offset,
                                                     (size_t)section->size - offset, &instruction, operands))) {
                size_t instruction_end = frida_padlock_instruction_end(elf, section, offset);
                if (instruction_end) {
                    fprintf(stderr, "confirmed Frida PadLock XSHA256 instruction sha256=%s section=%u file_offset=0x%llx bytes=%zu\n",
                            elf->sha256, s, (unsigned long long)(section->offset + offset), instruction_end - offset);
                    offset = instruction_end;
                    continue;
                }
                size_t data_end = frida_inline_data_end(elf, section, offset);
                if (data_end) {
                    fprintf(stderr, "confirmed Frida AES inline data sha256=%s section=%u file_offset=0x%llx bytes=%zu\n",
                            elf->sha256, s, (unsigned long long)(section->offset + offset), data_end - offset);
                    offset = data_end;
                    continue;
                }
                *error = "undecodable executable section; adaptation stopped";
                fprintf(stderr, "decode gap sha256=%s section=%u pc=0x%llx file_offset=0x%llx remaining=%llu\n", elf->sha256, s,
                        (unsigned long long)(elf->bias + section->addr + offset),
                        (unsigned long long)(section->offset + offset), (unsigned long long)(section->size - offset));
                return 0;
            }
            int tls = 0;
            for (unsigned i = 0; i < instruction.operand_count; ++i) {
                if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
#ifdef _WIN64
                if (operands[i].mem.segment == ZYDIS_REGISTER_FS) tls = 1;
#else
                if (operands[i].mem.segment == ZYDIS_REGISTER_GS) tls = 1;
#endif
            }
#ifndef _WIN64
            for (unsigned i = 0; i < instruction.operand_count; ++i)
                if (operands[i].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[i].reg.value == ZYDIS_REGISTER_GS) tls = 1;
            /* The original manager contains an indexed GOT-relative switch
               table inside .text. Confirm the SUB / JMP pair and every table
               destination before excluding these data bytes from decoding. */
            if (got && instruction.mnemonic == ZYDIS_MNEMONIC_SUB && instruction.operand_count_visible == 2 &&
                operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                operands[1].mem.base == ZYDIS_REGISTER_EBX && operands[1].mem.index != ZYDIS_REGISTER_NONE && operands[1].mem.scale == 4) {
                uint32_t address = (uint32_t)(got + operands[1].mem.disp.value);
                if (address >= section->addr && address - section->addr < section->size) {
                    table_start = address - (size_t)section->addr;
                    table_end = table_start;
                    for (; table_end + 4 <= section->size; table_end += 4) {
                        uint32_t delta;
                        memcpy(&delta, elf->file + section->offset + table_end, 4);
                        uint32_t target = (uint32_t)got - delta;
                        if (target < section->addr || target - section->addr >= section->size) break;
                    }
                    after_sub = offset + instruction.length;
                    table_register = operands[0].reg.value;
                }
            } else if (offset == after_sub && table_end > table_start) {
                if (instruction.mnemonic != ZYDIS_MNEMONIC_JMP || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                    operands[0].reg.value != table_register || table_start < offset + instruction.length ||
                    table_end - table_start < 8) table_end = 0;
            }
#else
            (void)got; (void)after_sub; (void)table_register;
#endif
#ifdef _WIN64
            int syscall = instruction.mnemonic == ZYDIS_MNEMONIC_SYSCALL;
#else
            int syscall = 0;
#endif
            if (tls || syscall) {
                if (instruction.length < 2) return 0;
#ifndef _WIN64
                if (tls) {
                    uintptr_t pc = elf->bias + (uintptr_t)section->addr + offset;
                    if (memcmp((const void *)pc, elf->file + section->offset + offset, instruction.length)) {
                        *error = "TLS instruction changed before adaptation"; return 0;
                    }
                    int direct = direct_tls(elf, pc, &instruction, operands);
                    if (direct < 0) return 0;
                    if (direct) { ++direct_count; offset += instruction.length; continue; }
                }
#endif
                if (elf->trap_count == capacity) {
                    capacity = capacity ? capacity * 2 : 128;
                    void *memory = elf->traps ? HeapReAlloc(GetProcessHeap(), 0, elf->traps, capacity * sizeof(NrTrap)) :
                                               HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(NrTrap));
                    if (!memory) { *error = "cannot allocate adaptation records"; return 0; }
                    elf->traps = memory;
                }
                uintptr_t pc = elf->bias + (uintptr_t)section->addr + offset;
                NrTrap *trap = &elf->traps[elf->trap_count++];
                trap->pc = pc;
                trap->tls = (unsigned char)tls;
                memcpy(trap->original, (const void *)pc, 2);
                DWORD old, ignored;
                if (!VirtualProtect((void *)pc, 2, PAGE_EXECUTE_READWRITE, &old)) return 0;
                memcpy((void *)pc, "\x0f\x0b", 2);
                if (!VirtualProtect((void *)pc, 2, old, &ignored)) return 0;
                if (tls) ++tls_count; else ++syscall_count;
            }
            offset += instruction.length;
        }
    }
    if (!section_count) return 0;
    if (!nr_service_adapt(elf, error)) return 0;
    if (!nr_pvf_adapt(elf, error)) return 0;
    for (NrCodePage *page = elf->tls_pages; page; page = page->next) {
        DWORD old;
        if (!VirtualProtect(page, sizeof(*page), PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(GetCurrentProcess(), page->code, page->used)) return 0;
    }
    if (elf->trap_count > 1) qsort(elf->traps, elf->trap_count, sizeof(*elf->traps), trap_compare);
    if (!FlushInstructionCache(GetCurrentProcess(), elf->allocation, elf->allocation_size)) return 0;
    fprintf(stderr, "adapt sha256=%s sections=%zu syscall_traps=%zu tls_traps=%zu tls_direct=%zu\n",
            elf->sha256, section_count, syscall_count, tls_count, direct_count);
    *error = NULL;
    return 1;
}


const NrTrap *nr_syscall_site(const NrElf *elf, uintptr_t pc) {
    uintptr_t base = (uintptr_t)elf->allocation;
    if (!base || pc < base || pc - base >= elf->allocation_size) return NULL;
    size_t low = 0, high = elf->trap_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (elf->traps[middle].pc < pc) low = middle + 1;
        else high = middle;
    }
    return low < elf->trap_count && elf->traps[low].pc == pc ? &elf->traps[low] : NULL;
}

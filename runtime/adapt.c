#include "elf.h"
#include <Zydis/Decoder.h>
#include <stdio.h>
#include <string.h>

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

int nr_adapt(NrElf *elf, const char **error) {
    NrElfHeader *h = elf->header;
    *error = "execution adaptation currently requires valid executable sections";
    if (!h->shnum || h->shentsize != sizeof(NrElfSection) || h->shoff > elf->file_size ||
        (uint64_t)h->shnum * sizeof(NrElfSection) > elf->file_size - h->shoff)
        return 0;
    NrElfSection *sections = (NrElfSection *)(elf->file + h->shoff);
    ZydisDecoder decoder;
#ifdef _WIN64
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif
    size_t capacity = 0, tls_count = 0, syscall_count = 0, section_count = 0;
    uintptr_t got = got_address(elf);
    for (unsigned s = 0; s < h->shnum; ++s) {
        NrElfSection *section = &sections[s];
        if (section->type != 1 || (section->flags & 6) != 6 || !section->size) continue;
        if (section->offset > elf->file_size || section->size > elf->file_size - section->offset)
            return 0;
        int mapped = 0;
        for (unsigned i = 0; i < h->phnum; ++i) {
            NrElfProgram *p = &elf->programs[i];
            if (p->type == 1 && (p->flags & 1) && section->addr >= p->vaddr &&
                section->addr - p->vaddr <= p->filesz &&
                section->size <= p->filesz - (section->addr - p->vaddr) &&
                section->offset == p->offset + (section->addr - p->vaddr))
                mapped = 1;
        }
        if (!mapped) return 0;
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
                *error = "undecodable executable section; adaptation stopped";
                fprintf(stderr, "decode gap section=%u pc=0x%llx file_offset=0x%llx remaining=%llu\n", s,
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
    if (!FlushInstructionCache(GetCurrentProcess(), elf->allocation, elf->allocation_size)) return 0;
    fprintf(stderr, "adapt sha256=%s sections=%zu syscall_traps=%zu tls_traps=%zu\n",
            elf->sha256, section_count, syscall_count, tls_count);
    *error = NULL;
    return 1;
}


int nr_syscall_site(const NrElf *elf, uintptr_t pc, int *tls) {
    for (size_t i = 0; i < elf->trap_count; ++i) {
        if (elf->traps[i].pc == pc) {
            *tls = elf->traps[i].tls;
            return 1;
        }
    }
    return 0;
}

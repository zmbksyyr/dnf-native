#include "run.h"
#include <Zydis/Decoder.h>
#include <Zydis/Encoder.h>
#include <string.h>

#ifdef _WIN64
#define MODE ZYDIS_MACHINE_MODE_LONG_64
#define WIDTH ZYDIS_STACK_WIDTH_64
#define SEGMENT ZYDIS_REGISTER_FS
#define PC(c) ((c)->Rip)
#define FIRST_GPR ZYDIS_REGISTER_RAX
#define LAST_GPR ZYDIS_REGISTER_R15
static DWORD64 *reg(CONTEXT *c, unsigned i) {
    DWORD64 *const registers[] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rsp, &c->Rbp, &c->Rsi, &c->Rdi,
                                 &c->R8, &c->R9, &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15};
    return i < 16 ? registers[i] : NULL;
}
#else
#define MODE ZYDIS_MACHINE_MODE_LEGACY_32
#define WIDTH ZYDIS_STACK_WIDTH_32
#define SEGMENT ZYDIS_REGISTER_GS
#define PC(c) ((c)->Eip)
#define FIRST_GPR ZYDIS_REGISTER_EAX
#define LAST_GPR ZYDIS_REGISTER_EDI
static DWORD *reg(CONTEXT *c, unsigned i) {
    DWORD *const registers[] = {&c->Eax, &c->Ecx, &c->Edx, &c->Ebx, &c->Esp, &c->Ebp, &c->Esi, &c->Edi};
    return i < 8 ? registers[i] : NULL;
}
#endif

static uintptr_t value(CONTEXT *c, ZydisRegister r, uintptr_t next) {
    if (r == ZYDIS_REGISTER_NONE) return 0;
    if (r == ZYDIS_REGISTER_RIP || r == ZYDIS_REGISTER_EIP) return next;
    ZydisRegister largest = ZydisRegisterGetLargestEnclosing(MODE, r);
    if (largest < FIRST_GPR || largest > LAST_GPR) return 0;
    uintptr_t v = (uintptr_t)*reg(c, largest - FIRST_GPR);
    unsigned width = ZydisRegisterGetWidth(MODE, r);
    return width < sizeof(uintptr_t) * 8 ? v & (((uintptr_t)1 << width) - 1) : v;
}

static int code_buffer(NrThread *thread, uintptr_t address) {
#ifdef _WIN64
    if (thread->tls_code) {
        intptr_t distance = (intptr_t)(address - (uintptr_t)thread->tls_code);
        if (distance > INT32_MIN + 4096 && distance < INT32_MAX - 4096) return 1;
        VirtualFree(thread->tls_code, 0, MEM_RELEASE);
        thread->tls_code = NULL;
    }
    uintptr_t cursor = address & ~(uintptr_t)65535;
    while (cursor <= address || cursor - address < 0x70000000ULL) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)cursor, &info, sizeof(info))) break;
        if (info.State == MEM_FREE) {
            thread->tls_code = VirtualAlloc((void *)cursor, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (thread->tls_code) return 1;
            /* Another thread can reserve this address after VirtualQuery.
               Do not skip the entire formerly free region using stale info;
               its remaining pages may be our only RIP-relative allocation. */
            if (GetLastError() != ERROR_INVALID_ADDRESS) return 0;
            cursor += 65536;
            continue;
        }
        uintptr_t next = ((uintptr_t)info.BaseAddress + info.RegionSize + 65535) & ~(uintptr_t)65535;
        if (next <= cursor) break;
        cursor = next;
    }
    return 0;
#else
    (void)address;
    if (!thread->tls_code) thread->tls_code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return thread->tls_code != NULL;
#endif
}

int nr_tls(NrRun *run, CONTEXT *c, const NrTrap *trap) {
    (void)run;
    NrThread *thread = nr_current();
    uintptr_t pc = PC(c);
    if (!thread->tls_base || !nr_memory(pc, 15, 0)) return 0;
    unsigned char bytes[15];
    memcpy(bytes, (const void *)pc, sizeof(bytes));
    if (trap) memcpy(bytes, trap->original, 2);
    ZydisDecoder decoder;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZydisDecoderInit(&decoder, MODE, WIDTH);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, sizeof(bytes), &instruction, operands))) return 0;
    uintptr_t next = pc + instruction.length;
#ifndef _WIN64
    if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.operand_count_visible == 2) {
        if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].reg.value == ZYDIS_REGISTER_GS &&
            operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && value(c, operands[1].reg.value, 0) == 0x63) {
            PC(c) = next;
            return 1;
        }
        if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[1].reg.value == ZYDIS_REGISTER_GS &&
            operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            ZydisRegister largest = ZydisRegisterGetLargestEnclosing(MODE, operands[0].reg.value);
            if (largest >= FIRST_GPR && largest <= LAST_GPR) {
                DWORD *out = reg(c, largest - FIRST_GPR);
                *out = operands[0].size == 16 ? (*out & 0xffff0000u) | 0x63 : 0x63;
                PC(c) = next;
                return 1;
            }
        }
    }
#endif
    int memory = -1;
    for (unsigned i = 0; i < instruction.operand_count; ++i) {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[i].mem.segment == SEGMENT) {
            if (memory >= 0 || i >= instruction.operand_count_visible) return 0;
            memory = (int)i;
        }
    }
    if (memory < 0 || instruction.mnemonic == ZYDIS_MNEMONIC_LEA) return 0;
    int call = instruction.meta.category == ZYDIS_CATEGORY_CALL;
    int jump = instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
    if (instruction.meta.category == ZYDIS_CATEGORY_RET || instruction.meta.category == ZYDIS_CATEGORY_COND_BR) return 0;
    ZydisDecodedOperand *operand = &operands[memory];
    uintptr_t address = value(c, operand->mem.base, next) + value(c, operand->mem.index, next) * operand->mem.scale +
                        (uintptr_t)operand->mem.disp.value;
    if (instruction.address_width == 32) address = (uint32_t)address;
    else if (instruction.address_width == 16) address = (uint16_t)address;
    address += thread->tls_base;
    if (!nr_memory(address, (operand->size + 7u) / 8u, (operand->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) ||
        (call && !nr_memory(value(c, (ZydisRegister)(FIRST_GPR + 4), 0) - sizeof(uintptr_t), sizeof(uintptr_t), 1)) ||
        !code_buffer(thread, address)) return 0;
    ZydisEncoderRequest request;
    if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(&instruction, operands, instruction.operand_count_visible, &request))) return 0;
    request.prefixes &= ~ZYDIS_ATTRIB_HAS_SEGMENT;
    request.operands[memory].mem.index = ZYDIS_REGISTER_NONE;
    request.operands[memory].mem.scale = 0;
    request.operands[memory].mem.displacement = (ZyanI64)address;
#ifdef _WIN64
    request.operands[memory].mem.base = ZYDIS_REGISTER_RIP;
    request.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_64;
    size_t prefix = call ? 6 : 0;
#else
    request.operands[memory].mem.base = ZYDIS_REGISTER_NONE;
    request.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_32;
    size_t prefix = call ? 5 : 0;
#endif
    if (call) request.mnemonic = ZYDIS_MNEMONIC_JMP;
    unsigned char code[64];
    ZyanUSize length = 15;
    if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, code + prefix, &length, (uintptr_t)thread->tls_code + prefix))) return 0;
    size_t count = prefix + length;
    if (call) {
#ifdef _WIN64
        code[0] = 0xff; code[1] = 0x35;
        uint32_t displacement = (uint32_t)length;
        memcpy(code + 2, &displacement, 4);
        memcpy(code + prefix + length, &next, sizeof(next));
        count += sizeof(next);
#else
        code[0] = 0x68; memcpy(code + 1, &next, 4);
#endif
    } else if (!jump) {
        /* Return natively without changing registers, flags or guest stack. */
#ifdef _WIN64
        memcpy(code + count, "\xff\x25\0\0\0\0", 6);
        memcpy(code + count + 6, &next, sizeof(next));
        count += 6 + sizeof(next);
#else
        code[count] = 0xe9;
        uint32_t displacement = (uint32_t)(next - ((uintptr_t)thread->tls_code + count + 5));
        memcpy(code + count + 1, &displacement, 4);
        count += 5;
#endif
    }
    /* This buffer belongs to the executing guest thread; it is never shared
       with another thread or exposed as a guest mapping. */
    memcpy(thread->tls_code, code, count);
    if (!FlushInstructionCache(GetCurrentProcess(), thread->tls_code, count)) return 0;
    PC(c) = (uintptr_t)thread->tls_code;
    return 1;
}

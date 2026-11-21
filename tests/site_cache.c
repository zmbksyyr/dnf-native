/* Exercise ownership, released tables and concurrent colliding trap lookups. */
#include "elf.h"
#include <stdio.h>

static DWORD WINAPI collide(void *context) {
    NrElf *elf = context;
    for (unsigned round = 0; round < 100000; ++round) {
        size_t index = round % elf->trap_count;
        if (nr_syscall_site(elf, elf->traps[index].pc) != &elf->traps[index]) return 1;
        if (nr_syscall_site(elf, (uintptr_t)elf->allocation + 2)) return 2;
    }
    return 0;
}

int main(void) {
    const uintptr_t base = 0x100000;
    NrTrap *released = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!released) return 1;
    released[0].pc = base + 4;
    NrElf first = {0};
    first.allocation = (unsigned char *)base;
    first.allocation_size = 4096;
    first.traps = released;
    first.trap_count = 1;
    if (nr_syscall_site(&first, base + 4) != released) return 2;
    if (!VirtualFree(released, 0, MEM_RELEASE)) return 3;

    /* A hint from an unloaded ELF must never be dereferenced by another ELF.
       Cache invalidation alone cannot ensure this with concurrent readers. */
    NrTrap second_traps[] = {{base + 4, {0, 0}, 1}, {base + 8, {0, 0}, 0}};
    NrElf second = first;
    second.traps = second_traps;
    second.trap_count = 2;
    if (nr_syscall_site(&second, base + 4) != &second_traps[0]) return 4;
    if (nr_syscall_site(&second, base + 8) != &second_traps[1]) return 5;
    second.trap_count = 0;
    if (nr_syscall_site(&second, base + 8)) return 6;
    if (nr_syscall_site(&second, base - 1) || nr_syscall_site(&second, base + 4096)) return 7;

    NrElf elves[4] = {0};
    NrTrap traps[4][8] = {0};
    HANDLE threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        uintptr_t address = base + (uintptr_t)i * 16384;
        elves[i].allocation = (unsigned char *)address;
        elves[i].allocation_size = 8192;
        elves[i].traps = traps[i];
        elves[i].trap_count = i + 1;
        for (unsigned j = 0; j <= i; ++j) traps[i][j].pc = address + 4 + (uintptr_t)j * 1024;
        threads[i] = CreateThread(NULL, 0, collide, &elves[i], 0, NULL);
        if (!threads[i]) return 8;
    }
    if (WaitForMultipleObjects(4, threads, TRUE, 30000) != WAIT_OBJECT_0) return 9;
    for (unsigned i = 0; i < 4; ++i) {
        DWORD result;
        if (!GetExitCodeThread(threads[i], &result) || result) return 10;
        CloseHandle(threads[i]);
    }
    puts("site-cache-ownership-and-collisions-ok");
    return 0;
}

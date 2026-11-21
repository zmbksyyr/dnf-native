#include "elf.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define NR_SITE_CACHE_SIZE 256
/* Cache table indices, never pointers into another ELF's lifetime. A hit is
   checked against the current ELF before returning its own trap record. */
static _Atomic(size_t) nr_site_cache[NR_SITE_CACHE_SIZE];
/* Off only for measurement: NR_SITE_CACHE_OFF=1 forces the binary search path. */
static atomic_int nr_site_cache_enabled = -1;

/* Self-measurement for the site cache: time the same lookups with the cache on and
   off so the change carries its own evidence. Enabled by NR_SITE_BENCH=1. */
void nr_site_cache_bench(const NrElf *elf) {
    const char *flag = getenv("NR_SITE_BENCH");
    if (!flag || flag[0] != '1' || elf->trap_count < 8) return;
    size_t take = elf->trap_count < 64 ? elf->trap_count : 64;
    LARGE_INTEGER freq, start, stop;
    QueryPerformanceFrequency(&freq);
    double results[2];
    int previous = atomic_load_explicit(&nr_site_cache_enabled, memory_order_relaxed);
    for (int pass = 0; pass < 2; ++pass) {
        int enabled = pass == 0;
        atomic_store_explicit(&nr_site_cache_enabled, enabled, memory_order_relaxed);
        for (size_t i = 0; i < take; ++i) nr_syscall_site(elf, elf->traps[i].pc);
        QueryPerformanceCounter(&start);
        for (int round = 0; round < 20000; ++round) {
            for (size_t i = 0; i < take; ++i) nr_syscall_site(elf, elf->traps[i].pc);
        }
        QueryPerformanceCounter(&stop);
        double total = (double)(stop.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        results[pass] = total * 1e9 / (20000.0 * (double)take);
    }
    atomic_store_explicit(&nr_site_cache_enabled, previous, memory_order_relaxed);
    fprintf(stderr, "site_cache_bench lookups=%zu cache_ns=%.1f bsearch_ns=%.1f speedup=%.2fx\n",
            take, results[0], results[1], results[1] / (results[0] > 0 ? results[0] : 1));
}

const NrTrap *nr_syscall_site(const NrElf *elf, uintptr_t pc) {
    uintptr_t base = (uintptr_t)elf->allocation;
    if (!base || pc < base || pc - base >= elf->allocation_size) return NULL;
    int enabled = atomic_load_explicit(&nr_site_cache_enabled, memory_order_relaxed);
    if (enabled < 0) {
        const char *off = getenv("NR_SITE_CACHE_OFF");
        enabled = !(off && off[0] == '1');
        atomic_store_explicit(&nr_site_cache_enabled, enabled, memory_order_relaxed);
    }
    unsigned slot = (unsigned)((pc >> 2) & (NR_SITE_CACHE_SIZE - 1));
    if (enabled) {
        size_t index = atomic_load_explicit(&nr_site_cache[slot], memory_order_relaxed);
        if (index && index <= elf->trap_count && elf->traps[index - 1].pc == pc)
            return &elf->traps[index - 1];
    }
    size_t low = 0, high = elf->trap_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (elf->traps[middle].pc < pc) low = middle + 1;
        else high = middle;
    }
    const NrTrap *found = low < elf->trap_count && elf->traps[low].pc == pc ? &elf->traps[low] : NULL;
    if (found && enabled) atomic_store_explicit(&nr_site_cache[slot], low + 1, memory_order_relaxed);
    return found;
}

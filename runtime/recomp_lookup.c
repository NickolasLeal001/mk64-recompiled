// Address -> recompiled function, for jalr and function pointers (table generated in lookup.c).
#include "recomp.h"

extern const struct {
    uint32_t addr;
    RecompFunc fn;
} recomp_func_table[];
extern const unsigned recomp_func_count;

RecompFunc recomp_lookup(uint32_t addr) {
    unsigned lo = 0, hi = recomp_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (recomp_func_table[mid].addr < addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < recomp_func_count && recomp_func_table[lo].addr == addr) {
        return recomp_func_table[lo].fn;
    }
    recomp_unhandled(addr, "call to an address that is not a function");
    return 0;
}

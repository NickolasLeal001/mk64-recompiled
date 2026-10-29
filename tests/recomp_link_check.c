// Links every recompiled function and checks the address table is sorted and complete.
#include <stdio.h>

#include "recomp.h"

extern const struct {
    uint32_t addr;
    RecompFunc fn;
} recomp_func_table[];
extern const unsigned recomp_func_count;

int main(void) {
    unsigned bad = 0;
    for (unsigned k = 1; k < recomp_func_count; k++) {
        if (recomp_func_table[k - 1].addr >= recomp_func_table[k].addr) bad++;
    }
    if (recomp_lookup(0x80000400u) == 0 || recomp_lookup(0x800cdc30u) == 0) bad++;
    printf("%u funcoes ligadas; tabela %s\n", recomp_func_count, bad ? "COM ERROS" : "ok");
    return bad != 0;
}

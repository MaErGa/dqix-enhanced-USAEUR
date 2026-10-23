#pragma once
#include "cpu.h"

/* One entry per recompiled function. ovl_id is -1 for code that is always
 * resident (main ARM9 binary, ITCM, DTCM). addr has bit 0 set for Thumb. */
typedef struct RecompFunc {
    int ovl_id;
    uint32_t addr;
    uint32_t size;
    void (*fn)(Cpu* c);
    const char* name;
} RecompFunc;

extern const RecompFunc g_recomp_funcs[];
extern const unsigned g_recomp_func_count;

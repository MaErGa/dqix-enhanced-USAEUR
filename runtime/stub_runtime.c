/*
 * Placeholder runtime: just enough to link the recompiled game.
 * The real runtime (memory map, graphics, sound, BIOS, overlays) replaces this.
 */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "dispatch.h"

static uint8_t g_ram[4 * 1024 * 1024];

static uint8_t* ptr(uint32_t a) { return &g_ram[a & (sizeof(g_ram) - 1)]; }
uint8_t  mem_read8(uint32_t a) { return *ptr(a); }
uint16_t mem_read16(uint32_t a) { uint16_t v; memcpy(&v, ptr(a & ~1u), 2); return v; }
uint32_t mem_read32(uint32_t a) { uint32_t v; memcpy(&v, ptr(a & ~3u), 4); return v; }
void mem_write8(uint32_t a, uint8_t v) { *ptr(a) = v; }
void mem_write16(uint32_t a, uint16_t v) { memcpy(ptr(a & ~1u), &v, 2); }
void mem_write32(uint32_t a, uint32_t v) { memcpy(ptr(a & ~3u), &v, 4); }

void cpu_write_cpsr_ctl(Cpu* c, uint32_t v, uint32_t m) { c->cpsr_ctl = (c->cpsr_ctl & ~m) | (v & m); }
void cpu_write_spsr(Cpu* c, uint32_t v, uint32_t m) { (void)c; (void)v; (void)m; }
uint32_t cpu_read_spsr(Cpu* c) { (void)c; return 0; }
void cp15_write(Cpu* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t v) { (void)c; (void)a; (void)b; (void)d; (void)e; (void)v; }
uint32_t cp15_read(Cpu* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) { (void)c; (void)a; (void)b; (void)d; (void)e; return 0; }
void bios_swi(Cpu* c, uint32_t n) { (void)c; fprintf(stderr, "swi %u\n", n); }
void cpu_breakpoint(Cpu* c, uint32_t a, uint32_t i) { (void)c; fprintf(stderr, "bkpt %08x %u\n", a, i); }
void cpu_unhandled(Cpu* c, uint32_t a, uint32_t w) { c->fault_addr = a; fprintf(stderr, "unhandled %08x: %08x\n", a, w); }
void cpu_exception_return(Cpu* c, uint32_t a) { (void)c; (void)a; }

static int g_loaded[64];
uint32_t g_cyc;
void recomp_budget_expired(Cpu* c) { (void)c; g_cyc = 0; }
void recomp_loop_expired(Cpu* c) { (void)c; g_cyc = 0; }
int overlay_is_loaded(int id) { return id >= 0 && id < 64 && g_loaded[id]; }

void dispatch_call(Cpu* c, uint32_t target, uint32_t from) {
    for (unsigned i = 0; i < g_recomp_func_count; i++) {
        const RecompFunc* f = &g_recomp_funcs[i];
        if ((f->addr & ~1u) == (target & ~1u) && (f->ovl_id < 0 || overlay_is_loaded(f->ovl_id))) {
            f->fn(c);
            return;
        }
    }
    fprintf(stderr, "dispatch: no function at %08x (from %08x)\n", target, from);
    c->fault_addr = target;
}

int main(void) {
    printf("%u recompiled functions linked.\n", g_recomp_func_count);
    return 0;
}

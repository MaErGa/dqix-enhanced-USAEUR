/*
 * DQIX recompiler: CPU state and helpers used by the generated C code.
 *
 * Every recompiled function has the signature `void fn(Cpu* c)`.
 * Arguments and return values travel in c->r[] exactly as on the ARM9,
 * so recompiled, hand-decompiled and runtime (HLE) functions can call
 * each other freely.
 */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct Cpu {
    uint32_t r[16];
    /* Condition flags, each stored as 0 or 1. */
    uint32_t n, z, cf, v, q;
    /* Non-flag CPSR bits (mode, I/F masks, T). */
    uint32_t cpsr_ctl;
    /* Set when a recompiled function hits something it cannot handle. */
    uint32_t fault_addr;
    /* Banked r13/r14/SPSR per CPU mode (index from cpu_mode_index()).
     * Only the runtime touches these; generated code never does. */
    uint32_t bank_r13[6], bank_r14[6], spsr[6];
} Cpu;

/* ---- services the runtime provides ---------------------------------- */
uint8_t  mem_read8(uint32_t addr);
uint16_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);
void     mem_write8(uint32_t addr, uint8_t v);
void     mem_write16(uint32_t addr, uint16_t v);
void     mem_write32(uint32_t addr, uint32_t v);

void     cpu_write_cpsr_ctl(Cpu* c, uint32_t value, uint32_t mask);
void     cpu_write_spsr(Cpu* c, uint32_t value, uint32_t mask);
uint32_t cpu_read_spsr(Cpu* c);
void     cp15_write(Cpu* c, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t value);
uint32_t cp15_read(Cpu* c, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2);
void     bios_swi(Cpu* c, uint32_t number);
void     cpu_breakpoint(Cpu* c, uint32_t addr, uint32_t imm);
void     cpu_unhandled(Cpu* c, uint32_t addr, uint32_t insn);
void     cpu_exception_return(Cpu* c, uint32_t addr);

/* Indirect control flow: find the recompiled function at `target`
 * (bit 0 = Thumb) taking currently loaded overlays into account. */
void     dispatch_call(Cpu* c, uint32_t target, uint32_t from);
int      overlay_is_loaded(int id);

/* ---- helpers --------------------------------------------------------- */
#define CPSR_GET(c) (((c)->n << 31) | ((c)->z << 30) | ((c)->cf << 29) | ((c)->v << 28) | ((c)->q << 27) | ((c)->cpsr_ctl & 0x07FFFFFFu))

static inline uint32_t ror32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

/* Register-specified shifts (shift amount = bottom byte of Rs). */
static inline uint32_t lsl_r(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v << s; }
static inline uint32_t lsr_r(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? 0 : v >> s; }
static inline uint32_t asr_r(uint32_t v, uint32_t s) { s &= 0xFF; return s >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> s); }
static inline uint32_t ror_r(uint32_t v, uint32_t s) { return ror32(v, s & 0xFF); }

/* Same, also returning the shifter carry-out. */
static inline uint32_t lsl_rc(uint32_t v, uint32_t s, uint32_t cin, uint32_t* co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    if (s < 32) { *co = (v >> (32 - s)) & 1; return v << s; }
    *co = (s == 32) ? (v & 1) : 0; return 0;
}
static inline uint32_t lsr_rc(uint32_t v, uint32_t s, uint32_t cin, uint32_t* co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    if (s < 32) { *co = (v >> (s - 1)) & 1; return v >> s; }
    *co = (s == 32) ? (v >> 31) : 0; return 0;
}
static inline uint32_t asr_rc(uint32_t v, uint32_t s, uint32_t cin, uint32_t* co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    if (s < 32) { *co = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *co = v >> 31; return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_rc(uint32_t v, uint32_t s, uint32_t cin, uint32_t* co) {
    s &= 0xFF;
    if (s == 0) { *co = cin; return v; }
    s &= 31;
    if (s == 0) { *co = v >> 31; return v; }
    *co = (v >> (s - 1)) & 1; return ror32(v, s);
}

/* Arithmetic with flag updates. */
static inline uint32_t add_s(Cpu* c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->n = r >> 31; c->z = r == 0; c->cf = r < a; c->v = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
static inline uint32_t adc_s(Cpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t w = (uint64_t)a + b + cin; uint32_t r = (uint32_t)w;
    c->n = r >> 31; c->z = r == 0; c->cf = (uint32_t)(w >> 32); c->v = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
static inline uint32_t sub_s(Cpu* c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->n = r >> 31; c->z = r == 0; c->cf = a >= b; c->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t sbc_s(Cpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint32_t borrow = !cin; uint32_t r = a - b - borrow;
    c->n = r >> 31; c->z = r == 0; c->cf = (uint64_t)a >= (uint64_t)b + borrow; c->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t nz(Cpu* c, uint32_t r) { c->n = r >> 31; c->z = r == 0; return r; }

/* ARMv5 LDR from an unaligned address rotates the loaded word. */
static inline uint32_t ldr_word(uint32_t addr) { return ror32(mem_read32(addr & ~3u), (addr & 3) * 8); }

/* DSP extension helpers (ARMv5TE). */
static inline uint32_t sat_add(Cpu* c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a + (int32_t)b;
    if (r > 0x7FFFFFFF) { c->q = 1; return 0x7FFFFFFF; }
    if (r < -0x7FFFFFFFLL - 1) { c->q = 1; return 0x80000000u; }
    return (uint32_t)r;
}
static inline uint32_t sat_sub(Cpu* c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a - (int32_t)b;
    if (r > 0x7FFFFFFF) { c->q = 1; return 0x7FFFFFFF; }
    if (r < -0x7FFFFFFFLL - 1) { c->q = 1; return 0x80000000u; }
    return (uint32_t)r;
}
static inline uint32_t add_q(Cpu* c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    if (((a ^ r) & (b ^ r)) >> 31) c->q = 1;
    return r;
}
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline int32_t half(uint32_t v, int top) { return top ? (int32_t)v >> 16 : (int32_t)(int16_t)v; }

/* Time. Generated code adds the estimated ARM9 cycle cost of each straight-line
 * run of instructions to g_cyc (RECOMP_CYC). At every function entry and loop
 * iteration, once enough has built up, the runtime turns it into emulated time
 * (ARM9) or hands over to the other CPU (ARM7), so game code takes about as long
 * as on the DS and a CPU spinning on memory the other one writes makes progress.
 * ARM7 code renames these to its own counters. Build with -DRECOMP_TRACE to also
 * record the last functions entered. */
extern uint32_t g_cyc;
#ifndef CYC_SYNC
#define CYC_SYNC 512u
#endif
void recomp_budget_expired(Cpu* c);
void recomp_loop_expired(Cpu* c);
#ifdef RECOMP_TRACE
void recomp_trace_enter(uint32_t addr);
#define RECOMP_TRACE_HOOK(a) recomp_trace_enter(a)
#else
#define RECOMP_TRACE_HOOK(a) ((void)0)
#endif
/* Around calls listed in tags.txt: the 3D polygons drawn by the call carry
 * the tag (gpu3d.c), so the renderers can leave them out. */
int gx_tag_begin(int tag);
void gx_tag_end(int previous);
#define RECOMP_CYC(n) (g_cyc += (n))
#define RECOMP_ENTER(a) do { if (__builtin_expect(g_cyc >= CYC_SYNC, 0)) recomp_budget_expired(c); RECOMP_TRACE_HOOK(a); } while (0)
#define RECOMP_LOOP() do { if (__builtin_expect(g_cyc >= CYC_SYNC, 0)) recomp_loop_expired(c); } while (0)

#define COND_EQ (c->z)
#define COND_NE (!c->z)
#define COND_CS (c->cf)
#define COND_CC (!c->cf)
#define COND_MI (c->n)
#define COND_PL (!c->n)
#define COND_VS (c->v)
#define COND_VC (!c->v)
#define COND_HI (c->cf && !c->z)
#define COND_LS (!c->cf || c->z)
#define COND_GE (c->n == c->v)
#define COND_LT (c->n != c->v)
#define COND_GT (!c->z && c->n == c->v)
#define COND_LE (c->z || c->n != c->v)

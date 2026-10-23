/* ARMv5TE interpreter (ARM + Thumb) for code that isn't recompiled: code the
 * game decrypts or writes at run time (DQIX's anti-piracy in overlay 29 is
 * encrypted in the ROM and decrypted in place before it runs).
 *
 * interp_call() runs from `target` until control comes back to the return
 * address that was in lr on entry. Whenever interpreted code reaches the
 * entry of a recompiled function it calls the native version instead. */
#include <string.h>
#include "nds.h"
#include "dispatch.h"

/* from dispatch.c */
const RecompFunc* dispatch_find(Core* k, uint32_t addr);

typedef struct { Core* k; Cpu* c; const Bus* b; uint32_t pc; int thumb; uint32_t cur; } St;

static uint32_t sx(uint32_t v, int bits) { uint32_t m = 1u << (bits - 1); return (v ^ m) - m; }
static uint32_t R(St* s, int r, int extra) { return r == 15 ? s->cur + (s->thumb ? 4 : 8) + extra : s->c->r[r]; }  /* cur = this instruction */

static int cond_ok(Cpu* c, uint32_t cond) {
    switch (cond) {
    case 0: return c->z; case 1: return !c->z; case 2: return c->cf; case 3: return !c->cf;
    case 4: return c->n; case 5: return !c->n; case 6: return c->v; case 7: return !c->v;
    case 8: return c->cf && !c->z; case 9: return !c->cf || c->z;
    case 10: return c->n == c->v; case 11: return c->n != c->v;
    case 12: return !c->z && c->n == c->v; case 13: return c->z || c->n != c->v;
    default: return 1;
    }
}

/* Control transfer. Returns 1 when the interpreted function has returned. */
static int jump(St* s, uint32_t target, uint32_t exit_lr);

static void do_call(St* s, uint32_t target, uint32_t ret) {
    /* bl/blx: lr = return address; native callee runs to completion */
    s->c->r[14] = ret;
    const RecompFunc* f = dispatch_find(s->k, target & ~1u);
    if (f) {
        f->fn(s->c);
        s->pc = ret & ~1u; s->thumb = ret & 1;
        return;
    }
    s->pc = target & ~1u; s->thumb = target & 1;
}

static int jump(St* s, uint32_t target, uint32_t exit_lr) {
    for (;;) {
        if ((target & ~1u) == exit_lr) return 1;
        const RecompFunc* f = dispatch_find(s->k, target & ~1u);
        if (!f) { s->pc = target & ~1u; s->thumb = target & 1; return 0; }
        /* tail call into recompiled code: it returns to the current lr */
        uint32_t tl = s->c->r[14];
        f->fn(s->c);
        target = tl;
    }
}

static uint32_t shift_imm(St* s, uint32_t w, uint32_t* carry) {
    uint32_t rm = w & 15, st = (w >> 5) & 3, amt = (w >> 7) & 31, v = R(s, rm, 0);
    Cpu* c = s->c;
    switch (st) {
    case 0: if (!amt) { *carry = c->cf; return v; } *carry = (v >> (32 - amt)) & 1; return v << amt;
    case 1: if (!amt) { *carry = v >> 31; return 0; } *carry = (v >> (amt - 1)) & 1; return v >> amt;
    case 2: if (!amt) { *carry = v >> 31; return (uint32_t)((int32_t)v >> 31); } *carry = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt);
    default: if (!amt) { *carry = v & 1; return (c->cf << 31) | (v >> 1); } *carry = (v >> (amt - 1)) & 1; return ror32(v, amt);
    }
}

static uint32_t operand2(St* s, uint32_t w, uint32_t* carry, int* regshift) {
    Cpu* c = s->c;
    *regshift = 0;
    if ((w >> 25) & 1) {
        uint32_t rot = ((w >> 8) & 15) * 2, v = ror32(w & 0xFF, rot);
        *carry = rot ? v >> 31 : c->cf;
        return v;
    }
    if (!((w >> 4) & 1)) return shift_imm(s, w, carry);
    *regshift = 1;
    uint32_t v = R(s, w & 15, 4), sh = R(s, (w >> 8) & 15, 4);
    switch ((w >> 5) & 3) {
    case 0: return lsl_rc(v, sh, c->cf, carry);
    case 1: return lsr_rc(v, sh, c->cf, carry);
    case 2: return asr_rc(v, sh, c->cf, carry);
    default: return ror_rc(v, sh, c->cf, carry);
    }
}

static uint32_t load(St* s, uint32_t a, int kind) {
    const Bus* b = s->b;
    switch (kind) {
    case 0: return ror32(b->r32(a & ~3u), (a & 3) * 8);       /* word */
    case 1: return b->r8(a);
    case 2: return b->r16(a);
    case 3: return sx(b->r8(a), 8);
    default: return sx(b->r16(a), 16);
    }
}

/* Write to pc from a data-processing / load instruction. */
static int write_pc(St* s, uint32_t v, uint32_t exit_lr, int interwork) {
    if (!interwork) v = (v & ~1u) | (s->thumb ? 1 : 0);
    return jump(s, v, exit_lr);
}

/* ---------------------------------------------------------------- ARM */
static int step_arm(St* s, uint32_t exit_lr) {
    Cpu* c = s->c; const Bus* b = s->b;
    uint32_t a = s->pc, w = b->r32(a);
    uint32_t cond = w >> 28;
    s->cur = a;
    s->pc = a + 4;
    if (cond == 0xF) {
        if (((w >> 25) & 7) == 5) {                                   /* blx imm */
            uint32_t t = a + 8 + (sx(w & 0xFFFFFF, 24) << 2) + (((w >> 24) & 1) << 1);
            do_call(s, t | 1, a + 4);
            return 0;
        }
        return 0;                                                     /* pld etc. */
    }
    if (!cond_ok(c, cond)) return 0;
    uint32_t op = (w >> 25) & 7;
    uint32_t rn = (w >> 16) & 15, rd = (w >> 12) & 15, rs = (w >> 8) & 15, rm = w & 15;

    if (op == 0 && (w & 0x90) == 0x90 && ((w >> 5) & 3) == 0) {       /* multiply / swap */
        if ((w >> 24) & 1) {                                          /* swp */
            uint32_t addr = c->r[rn];
            if ((w >> 22) & 1) { uint32_t t = b->r8(addr); b->w8(addr, (uint8_t)c->r[rm]); c->r[rd] = t; }
            else { uint32_t t = load(s, addr, 0); b->w32(addr & ~3u, c->r[rm]); c->r[rd] = t; }
            return 0;
        }
        uint32_t mop = (w >> 21) & 7, S = (w >> 20) & 1;
        uint32_t m = c->r[rm], sv = c->r[rs];
        if (mop <= 1) {                                               /* mul / mla: Rd = bits 16-19, Rn = bits 12-15 */
            uint32_t r = m * sv + (mop ? c->r[rd] : 0);
            c->r[rn] = r;
            if (S) nz(c, r);
            return 0;
        }
        uint32_t hi = rn, lo = rd;
        uint64_t p = mop >= 6 ? (uint64_t)((int64_t)(int32_t)m * (int32_t)sv) : (uint64_t)m * sv;
        if (mop == 5 || mop == 7) p += ((uint64_t)c->r[hi] << 32) | c->r[lo];
        c->r[lo] = (uint32_t)p; c->r[hi] = (uint32_t)(p >> 32);
        if (S) { c->n = (uint32_t)(p >> 63); c->z = p == 0; }
        return 0;
    }
    if (op == 0 && (w & 0x90) == 0x90) {                              /* halfword / signed / doubleword */
        uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, I = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        uint32_t sh = (w >> 5) & 3;
        uint32_t off = I ? (((w >> 4) & 0xF0) | (w & 15)) : R(s, rm, 0);
        uint32_t base = R(s, rn, 0);
        uint32_t ea = P ? (U ? base + off : base - off) : base;
        uint32_t nb = U ? base + off : base - off;
        int wb = (!P || W) && rn != 15;
        if (sh == 1 || L) {
            if (L) {
                uint32_t v = load(s, ea, sh == 1 ? 2 : sh == 2 ? 3 : 4);
                if (wb) c->r[rn] = nb;
                if (rd == 15) return write_pc(s, v, exit_lr, 1);
                c->r[rd] = v;
            } else {
                b->w16(ea, (uint16_t)R(s, rd, 4));
                if (wb) c->r[rn] = nb;
            }
            return 0;
        }
        if (sh == 2) { uint32_t v0 = b->r32(ea), v1 = b->r32(ea + 4); if (wb) c->r[rn] = nb; c->r[rd] = v0; c->r[rd + 1] = v1; }
        else { b->w32(ea, c->r[rd]); b->w32(ea + 4, c->r[rd + 1]); if (wb) c->r[rn] = nb; }
        return 0;
    }
    if ((op == 0 || op == 1) && ((w >> 23) & 3) == 2 && !((w >> 20) & 1)) {   /* misc */
        uint32_t lo4 = (w >> 4) & 15, mop = (w >> 21) & 3;
        if (op == 1 || lo4 == 0) {
            if (op == 0 && !(mop & 1)) { c->r[rd] = (w >> 22) & 1 ? cpu_read_spsr(c) : CPSR_GET(c); return 0; }  /* mrs */
            uint32_t v = op == 1 ? ror32(w & 0xFF, ((w >> 8) & 15) * 2) : c->r[rm];  /* msr */
            uint32_t mask = 0;
            for (int f = 0; f < 4; f++) if ((w >> (16 + f)) & 1) mask |= 0xFFu << (8 * f);
            if ((w >> 22) & 1) { cpu_write_spsr(c, v, mask); return 0; }
            if (mask & 0xF8000000) { c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
            if (mask & 0x07FFFFFF) cpu_write_cpsr_ctl(c, v, mask & 0x07FFFFFF);
            return 0;
        }
        if (lo4 == 1 && mop == 1) return jump(s, R(s, rm, 0), exit_lr);                 /* bx */
        if (lo4 == 1 && mop == 3) { c->r[rd] = clz32(c->r[rm]); return 0; }             /* clz */
        if (lo4 == 3 && mop == 1) { do_call(s, R(s, rm, 0), a + 4); return 0; }         /* blx reg */
        if (lo4 == 5) {
            uint32_t m = c->r[rm], n = c->r[rn];
            c->r[rd] = mop == 0 ? sat_add(c, m, n) : mop == 1 ? sat_sub(c, m, n)
                     : mop == 2 ? sat_add(c, m, sat_add(c, n, n)) : sat_sub(c, m, sat_add(c, n, n));
            return 0;
        }
        if (lo4 == 7) { cpu_breakpoint(c, a, 0); return 0; }
        if ((lo4 & 9) == 8) {                                         /* DSP multiplies */
            int x = (w >> 5) & 1, y = (w >> 6) & 1;
            int32_t mh = half(c->r[rm], x), sh = half(c->r[rs], y);
            switch (mop) {
            case 0: c->r[rn] = add_q(c, (uint32_t)(mh * sh), c->r[rd]); break;
            case 1: {
                uint32_t p = (uint32_t)(((int64_t)(int32_t)c->r[rm] * sh) >> 16);
                c->r[rn] = x ? p : add_q(c, p, c->r[rd]);
                break;
            }
            case 2: {
                uint64_t acc = ((uint64_t)c->r[rn] << 32) | c->r[rd];
                acc += (uint64_t)(int64_t)(mh * sh);
                c->r[rd] = (uint32_t)acc; c->r[rn] = (uint32_t)(acc >> 32);
                break;
            }
            default: c->r[rn] = (uint32_t)(mh * sh);
            }
            return 0;
        }
        rt_fatal("interp: unknown misc instruction %08x at %08x\n", w, a);
    }
    if (op == 0 || op == 1) {                                         /* data processing */
        uint32_t opc = (w >> 21) & 15, S = (w >> 20) & 1, carry; int regshift;
        uint32_t o2 = operand2(s, w, &carry, &regshift);
        uint32_t n = R(s, rn, regshift ? 4 : 0), r = 0, cin = c->cf;
        int logical = opc <= 1 || opc == 8 || opc == 9 || opc >= 12;
        switch (opc) {
        case 0: case 8: r = n & o2; break;
        case 1: case 9: r = n ^ o2; break;
        case 2: case 10: r = S ? sub_s(c, n, o2) : n - o2; break;
        case 3: r = S ? sub_s(c, o2, n) : o2 - n; break;
        case 4: case 11: r = S ? add_s(c, n, o2) : n + o2; break;
        case 5: r = S ? adc_s(c, n, o2, cin) : n + o2 + cin; break;
        case 6: r = S ? sbc_s(c, n, o2, cin) : n - o2 - !cin; break;
        case 7: r = S ? sbc_s(c, o2, n, cin) : o2 - n - !cin; break;
        case 12: r = n | o2; break;
        case 13: r = o2; break;
        case 14: r = n & ~o2; break;
        default: r = ~o2; break;
        }
        if (S && logical) { nz(c, r); c->cf = carry; }
        if (opc >= 8 && opc <= 11) return 0;
        if (rd == 15) {
            if (S) { cpu_exception_return(c, r); return jump(s, r, exit_lr); }
            return write_pc(s, r, exit_lr, 0);
        }
        c->r[rd] = r;
        return 0;
    }
    if (op == 2 || op == 3) {                                         /* ldr / str */
        uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, B = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1, carry;
        uint32_t off = op == 2 ? (w & 0xFFF) : shift_imm(s, w, &carry);
        uint32_t base = R(s, rn, 0);
        uint32_t ea = P ? (U ? base + off : base - off) : base;
        uint32_t nb = U ? base + off : base - off;
        int wb = (!P || W) && rn != 15;
        if (L) {
            uint32_t v = load(s, ea, B ? 1 : 0);
            if (wb) c->r[rn] = nb;
            if (rd == 15) return write_pc(s, v, exit_lr, 1);
            c->r[rd] = v;
        } else {
            uint32_t v = R(s, rd, 4);
            if (B) b->w8(ea, (uint8_t)v); else b->w32(ea & ~3u, v);
            if (wb) c->r[rn] = nb;
        }
        return 0;
    }
    if (op == 4) {                                                    /* ldm / stm */
        uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, S = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
        uint32_t list = w & 0xFFFF, n = (uint32_t)__builtin_popcount(list);
        uint32_t base = c->r[rn];
        uint32_t addr = U ? base + (P ? 4 : 0) : base - 4 * n + (P ? 0 : 4);
        uint32_t nb = U ? base + 4 * n : base - 4 * n;
        if (L) {
            uint32_t vals[16]; uint32_t k = 0;
            for (int r = 0; r < 16; r++) if (list >> r & 1) vals[r] = b->r32(addr + 4 * k++);
            if (W && !(list >> rn & 1)) c->r[rn] = nb;
            for (int r = 0; r < 15; r++) if (list >> r & 1) c->r[r] = vals[r];
            if (list >> 15 & 1) {
                if (S) cpu_exception_return(c, vals[15]);
                return write_pc(s, vals[15], exit_lr, 1);
            }
        } else {
            uint32_t k = 0;
            for (int r = 0; r < 16; r++) if (list >> r & 1) {
                uint32_t v = r == 15 ? a + 12 : c->r[r];
                if (r == (int)rn && k != 0 && W) v = nb;
                b->w32(addr + 4 * k++, v);
            }
            if (W) c->r[rn] = nb;
        }
        return 0;
    }
    if (op == 5) {                                                    /* b / bl */
        uint32_t t = a + 8 + (sx(w & 0xFFFFFF, 24) << 2);
        if ((w >> 24) & 1) { do_call(s, t, a + 4); return 0; }
        return jump(s, t, exit_lr);
    }
    if (op == 7 && (w >> 24) & 1) { (s->k->id ? bios7_swi : bios_swi)(c, (w >> 16) & 0xFF); return 0; }
    if (op == 7 && (w & 0x10) && ((w >> 8) & 15) == 15) {             /* mrc / mcr p15 */
        uint32_t op1 = (w >> 21) & 7, crn = (w >> 16) & 15, op2 = (w >> 5) & 7, crm = w & 15;
        if ((w >> 20) & 1) c->r[rd] = cp15_read(c, op1, crn, crm, op2);
        else cp15_write(c, op1, crn, crm, op2, R(s, rd, 4));
        return 0;
    }
    rt_fatal("interp: unknown instruction %08x at %08x\n", w, a);
}

/* ---------------------------------------------------------------- Thumb */
static int step_thumb(St* s, uint32_t exit_lr) {
    Cpu* c = s->c; const Bus* b = s->b;
    uint32_t a = s->pc, w = b->r16(a), pc = a + 4;
    s->cur = a;
    s->pc = a + 2;
    uint32_t top5 = w >> 11;
    if (top5 < 3) {
        uint32_t amt = (w >> 6) & 31, rsv = c->r[(w >> 3) & 7], rd = w & 7, cc;
        if (top5 == 0) { if (!amt) { c->r[rd] = nz(c, rsv); return 0; } cc = (rsv >> (32 - amt)) & 1; c->r[rd] = nz(c, rsv << amt); }
        else if (top5 == 1) { uint32_t n = amt ? amt : 32; cc = (rsv >> (n - 1)) & 1; c->r[rd] = nz(c, n == 32 ? 0 : rsv >> n); }
        else { uint32_t n = amt ? amt : 32; cc = (rsv >> (n - 1)) & 1; c->r[rd] = nz(c, (uint32_t)((int32_t)rsv >> (n > 31 ? 31 : n))); }
        c->cf = cc;
        return 0;
    }
    if (top5 == 3) {
        uint32_t o = (w >> 10) & 1 ? (w >> 6) & 7 : c->r[(w >> 6) & 7], sv = c->r[(w >> 3) & 7];
        c->r[w & 7] = (w >> 9) & 1 ? sub_s(c, sv, o) : add_s(c, sv, o);
        return 0;
    }
    if (top5 < 8) {
        uint32_t rd = (w >> 8) & 7, imm = w & 0xFF;
        switch ((w >> 11) & 3) {
        case 0: c->r[rd] = nz(c, imm); break;
        case 1: (void)sub_s(c, c->r[rd], imm); break;
        case 2: c->r[rd] = add_s(c, c->r[rd], imm); break;
        default: c->r[rd] = sub_s(c, c->r[rd], imm); break;
        }
        return 0;
    }
    if ((w >> 10) == 0x10) {
        uint32_t op = (w >> 6) & 15, rd = w & 7, d = c->r[rd], sv = c->r[(w >> 3) & 7], cc;
        switch (op) {
        case 0: c->r[rd] = nz(c, d & sv); break;
        case 1: c->r[rd] = nz(c, d ^ sv); break;
        case 2: c->r[rd] = nz(c, lsl_rc(d, sv, c->cf, &cc)); c->cf = cc; break;
        case 3: c->r[rd] = nz(c, lsr_rc(d, sv, c->cf, &cc)); c->cf = cc; break;
        case 4: c->r[rd] = nz(c, asr_rc(d, sv, c->cf, &cc)); c->cf = cc; break;
        case 5: c->r[rd] = adc_s(c, d, sv, c->cf); break;
        case 6: c->r[rd] = sbc_s(c, d, sv, c->cf); break;
        case 7: c->r[rd] = nz(c, ror_rc(d, sv, c->cf, &cc)); c->cf = cc; break;
        case 8: (void)nz(c, d & sv); break;
        case 9: c->r[rd] = sub_s(c, 0, sv); break;
        case 10: (void)sub_s(c, d, sv); break;
        case 11: (void)add_s(c, d, sv); break;
        case 12: c->r[rd] = nz(c, d | sv); break;
        case 13: c->r[rd] = nz(c, d * sv); break;
        case 14: c->r[rd] = nz(c, d & ~sv); break;
        default: c->r[rd] = nz(c, ~sv); break;
        }
        return 0;
    }
    if ((w >> 10) == 0x11) {
        uint32_t op = (w >> 8) & 3, rsn = (w >> 3) & 15, rd = (w & 7) | ((w >> 4) & 8);
        uint32_t sv = rsn == 15 ? pc : c->r[rsn];
        if (op == 3) {
            if ((w >> 7) & 1) { do_call(s, sv, (a + 2) | 1); return 0; }
            return jump(s, sv, exit_lr);
        }
        uint32_t d = rd == 15 ? pc : c->r[rd];
        if (op == 1) { (void)sub_s(c, d, sv); return 0; }
        uint32_t r = op == 0 ? d + sv : sv;
        if (rd == 15) return jump(s, r | 1, exit_lr);
        c->r[rd] = r;
        return 0;
    }
    if ((w >> 11) == 9) { c->r[(w >> 8) & 7] = b->r32((pc & ~3u) + (w & 0xFF) * 4); return 0; }
    if ((w >> 12) == 5) {
        uint32_t ea = c->r[(w >> 3) & 7] + c->r[(w >> 6) & 7], rd = w & 7;
        switch ((w >> 9) & 7) {
        case 0: b->w32(ea & ~3u, c->r[rd]); break;
        case 1: b->w16(ea, (uint16_t)c->r[rd]); break;
        case 2: b->w8(ea, (uint8_t)c->r[rd]); break;
        case 3: c->r[rd] = load(s, ea, 3); break;
        case 4: c->r[rd] = load(s, ea, 0); break;
        case 5: c->r[rd] = load(s, ea, 2); break;
        case 6: c->r[rd] = load(s, ea, 1); break;
        default: c->r[rd] = load(s, ea, 4); break;
        }
        return 0;
    }
    if ((w >> 13) == 3) {
        uint32_t B = (w >> 12) & 1, L = (w >> 11) & 1, off = (w >> 6) & 31, rd = w & 7;
        uint32_t ea = c->r[(w >> 3) & 7] + (B ? off : off * 4);
        if (L) c->r[rd] = load(s, ea, B ? 1 : 0);
        else if (B) b->w8(ea, (uint8_t)c->r[rd]); else b->w32(ea & ~3u, c->r[rd]);
        return 0;
    }
    if ((w >> 12) == 8) {
        uint32_t ea = c->r[(w >> 3) & 7] + ((w >> 6) & 31) * 2, rd = w & 7;
        if ((w >> 11) & 1) c->r[rd] = b->r16(ea); else b->w16(ea, (uint16_t)c->r[rd]);
        return 0;
    }
    if ((w >> 12) == 9) {
        uint32_t ea = c->r[13] + (w & 0xFF) * 4, rd = (w >> 8) & 7;
        if ((w >> 11) & 1) c->r[rd] = load(s, ea, 0); else b->w32(ea & ~3u, c->r[rd]);
        return 0;
    }
    if ((w >> 12) == 10) { c->r[(w >> 8) & 7] = ((w >> 11) & 1 ? c->r[13] : (pc & ~3u)) + (w & 0xFF) * 4; return 0; }
    if ((w >> 8) == 0xB0) { uint32_t imm = (w & 0x7F) * 4; c->r[13] += (w & 0x80) ? -imm : imm; return 0; }
    if ((w >> 12) == 0xB && ((w >> 9) & 3) == 2) {
        uint32_t list = w & 0xFF;
        if ((w >> 11) & 1) {                                          /* pop */
            uint32_t sp = c->r[13];
            for (int r = 0; r < 8; r++) if (list >> r & 1) { c->r[r] = b->r32(sp); sp += 4; }
            if ((w >> 8) & 1) { uint32_t v = b->r32(sp); sp += 4; c->r[13] = sp; return jump(s, v, exit_lr); }
            c->r[13] = sp;
        } else {                                                      /* push */
            uint32_t n = (uint32_t)__builtin_popcount(list) + ((w >> 8) & 1);
            uint32_t sp = c->r[13] - 4 * n, p = sp;
            for (int r = 0; r < 8; r++) if (list >> r & 1) { b->w32(p, c->r[r]); p += 4; }
            if ((w >> 8) & 1) b->w32(p, c->r[14]);
            c->r[13] = sp;
        }
        return 0;
    }
    if ((w >> 8) == 0xBE) { cpu_breakpoint(c, a, w & 0xFF); return 0; }
    if ((w >> 12) == 0xC) {
        uint32_t rb = (w >> 8) & 7, list = w & 0xFF, p = c->r[rb];
        if ((w >> 11) & 1) {
            for (int r = 0; r < 8; r++) if (list >> r & 1) { c->r[r] = b->r32(p); p += 4; }
            if (!(list >> rb & 1)) c->r[rb] = p;
        } else {
            for (int r = 0; r < 8; r++) if (list >> r & 1) { b->w32(p, c->r[r]); p += 4; }
            c->r[rb] = p;
        }
        return 0;
    }
    if ((w >> 12) == 0xD) {
        uint32_t cond = (w >> 8) & 15;
        if (cond == 15) { (s->k->id ? bios7_swi : bios_swi)(c, w & 0xFF); return 0; }
        if (cond_ok(c, cond)) return jump(s, (pc + (sx(w & 0xFF, 8) << 1)) | 1, exit_lr);
        return 0;
    }
    if (top5 == 0x1C) return jump(s, (pc + (sx(w & 0x7FF, 11) << 1)) | 1, exit_lr);
    if (top5 == 0x1E) {
        uint32_t nxt = b->r16(a + 2);
        uint32_t off = (sx(w & 0x7FF, 11) << 12) + ((nxt & 0x7FF) << 1);
        s->pc = a + 4;
        if ((nxt >> 11) == 0x1F) do_call(s, (pc + off) | 1, (a + 4) | 1);
        else do_call(s, (pc + off) & ~3u, (a + 4) | 1);
        return 0;
    }
    rt_fatal("interp: unknown Thumb instruction %04x at %08x\n", w, a);
}

unsigned long long g_interp_steps;

void interp_call(Core* k, uint32_t target) {
    St s = { k, k->cpu, k->bus, target & ~1u, (int)(target & 1), 0 };
    uint32_t exit_lr = k->cpu->r[14] & ~1u;
    LOG_DEBUG("%s interp: enter %08x (returns to %08x)\n", k->name, target, exit_lr);
    for (;;) {
        g_interp_steps++;
        if (s.thumb ? step_thumb(&s, exit_lr) : step_arm(&s, exit_lr)) return;
        if (s.pc == exit_lr) return;
    }
}

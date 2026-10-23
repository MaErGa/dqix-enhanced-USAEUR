/*
 * Test functions for the recompiler. Each is compiled twice: natively (the
 * reference) and to ARMv5TE/Thumb machine code, which is then recompiled to C.
 * Results must agree for every input.
 * All take/return 32-bit values; memory effects go through g_buf and are
 * folded into the return value.
 */
#include <stdint.h>

#define T __attribute__((noinline, used))
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

volatile u32 g_buf[64];
u32 g_state = 0x12345678;

T u32 t_add(u32 a, u32 b, u32 c, u32 d) { return a + b * 3 - c + (d << 5); }
T u32 t_logic(u32 a, u32 b, u32 c, u32 d) { return (a & ~b) ^ (c | d) ^ ~(a >> 3); }
T u32 t_shift_var(u32 a, u32 b, u32 c, u32 d) {
    u32 s = b & 63;
    u32 x = s < 32 ? a << s : 0;
    u32 y = s < 32 ? a >> s : 0;
    s32 z = s < 32 ? (s32)a >> s : ((s32)a >> 31);
    return x ^ (y * 7) ^ (u32)z ^ ((c >> (d & 31)) | (c << ((32 - (d & 31)) & 31)));
}
T u32 t_cmp_signed(u32 a, u32 b, u32 c, u32 d) {
    s32 x = (s32)a, y = (s32)b;
    u32 r = 0;
    if (x < y) r |= 1;
    if (x <= y) r |= 2;
    if (x > y) r |= 4;
    if (x >= y) r |= 8;
    if (x == y) r |= 16;
    if ((s32)c < 0) r |= 32;
    if ((s32)(c + d) < (s32)c) r |= 64;
    return r;
}
T u32 t_cmp_unsigned(u32 a, u32 b, u32 c, u32 d) {
    u32 r = 0;
    if (a < b) r |= 1;
    if (a <= b) r |= 2;
    if (a > b) r |= 4;
    if (a >= b) r |= 8;
    if (c + d < c) r |= 16;  /* carry */
    if (c - d > c) r |= 32;  /* borrow */
    return r;
}
T u32 t_minmax(u32 a, u32 b, u32 c, u32 d) {
    s32 x = (s32)a, y = (s32)b;
    s32 mn = x < y ? x : y;
    u32 umx = c > d ? c : d;
    s32 ab = x < 0 ? -x : x;
    return (u32)mn * 31 + umx + (u32)ab;
}
T u32 t_mul64_u(u32 a, u32 b, u32 c, u32 d) { u64 p = (u64)a * b + (((u64)c << 32) | d); return (u32)(p >> 32) ^ (u32)p; }
T u32 t_mul64_s(u32 a, u32 b, u32 c, u32 d) { s64 p = (s64)(s32)a * (s32)b + (s64)(s32)c * (s32)d; return (u32)(p >> 32) ^ (u32)p * 3; }
T u32 t_add64(u32 a, u32 b, u32 c, u32 d) {
    u64 x = ((u64)a << 32) | b, y = ((u64)c << 32) | d;
    u64 s = x + y, t = x - y;
    return (u32)(s >> 32) ^ (u32)s ^ (u32)(t >> 29) ^ (u32)(t * 5);
}
T u32 t_cmp64(u32 a, u32 b, u32 c, u32 d) {
    s64 x = ((s64)(s32)a << 32) | b, y = ((s64)(s32)c << 32) | d;
    return (x < y) | ((x == y) << 1) | (((u64)x > (u64)y) << 2);
}
T u32 t_fixed(u32 a, u32 b, u32 c, u32 d) {
    /* DS-style 20.12 fixed point */
    s32 x = (s32)a >> 8, y = (s32)b >> 8;
    s32 m = (s32)(((s64)x * y + 0x800) >> 12);
    return (u32)m + (u32)(((s64)(s32)c * (s32)d) >> 12);
}
T u32 t_div(u32 a, u32 b, u32 c, u32 d) {
    u32 r = 0;
    if (b) r += a / b + a % b * 17;
    if (d && !((s32)c == (s32)0x80000000 && (s32)d == -1)) r ^= (u32)((s32)c / (s32)d) + (u32)((s32)c % (s32)d) * 5;
    return r;
}
T u32 t_switch(u32 a, u32 b, u32 c, u32 d) {
    switch (a % 13) {
    case 0: return b + 1;
    case 1: return b ^ c;
    case 2: return c * 3;
    case 3: return d - b;
    case 4: return b << 2;
    case 5: return ~c;
    case 6: return b + c + d;
    case 7: return d >> 3;
    case 8: return 7;
    case 9: return b & d;
    case 10: return b | 0x80;
    case 11: return c - 9;
    default: return 0xdead;
    }
}
T u32 t_switch_sparse(u32 a, u32 b, u32 c, u32 d) {
    (void)c; (void)d;
    switch (a & 0x3ff) {
    case 3: return 1; case 100: return 2; case 101: return b; case 512: return 3;
    case 999: return 4; case 7: return 5; default: return 6;
    }
}
T u32 t_loop(u32 a, u32 b, u32 c, u32 d) {
    u32 h = 2166136261u ^ d;
    u32 n = (b & 63) + 1;
    for (u32 i = 0; i < n; i++) { h ^= (a >> (i & 31)) & 0xff; h *= 16777619u; h += c; }
    return h;
}
T u32 t_bits(u32 a, u32 b, u32 c, u32 d) {
    u32 pc = 0, x = a;
    while (x) { pc += x & 1; x >>= 1; }
    return pc + (u32)__builtin_clz(b | 1) * 64 + (u32)__builtin_ctz(c | 0x80000000) * 4096 + ((d >> 4) & 0xF0F0);
}
T u32 t_mem_bytes(u32 a, u32 b, u32 c, u32 d) {
    volatile uint8_t* p = (volatile uint8_t*)g_buf;
    volatile uint16_t* q = (volatile uint16_t*)g_buf;
    for (int i = 0; i < 64; i++) g_buf[i] = a * (u32)i + b;
    p[c & 255] = (uint8_t)d;
    q[(d >> 3) & 127] = (uint16_t)(a ^ c);
    u32 s = 0;
    for (int i = 0; i < 256; i++) s = s * 31 + p[i];
    for (int i = 0; i < 128; i++) s += (u32)(int16_t)q[i] * (u32)i;
    s += (u32)(int8_t)p[d & 255];
    return s;
}
typedef struct { u32 a, b, c, d, e, f, g; } Big;
T Big make_big(u32 a, u32 b) { Big x = { a, b, a ^ b, a + b, a - b, a * b, ~a }; return x; }
T u32 t_struct(u32 a, u32 b, u32 c, u32 d) {
    Big x = make_big(a, b), y = make_big(c, d);
    Big z = (a & 1) ? x : y;
    return z.a + z.b * 3 + z.c * 5 + z.d * 7 + z.e * 11 + z.f * 13 + z.g * 17;
}
T u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
T u32 t_recursion(u32 a, u32 b, u32 c, u32 d) { (void)c; (void)d; return fib(a % 18) + fib(b % 11); }
T u32 t_many_args_callee(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h; }
T u32 t_many_args(u32 a, u32 b, u32 c, u32 d) { return t_many_args_callee(a, b, c, d, a ^ b, c ^ d, a * c, b * d); }
typedef u32 (*op_fn)(u32, u32, u32, u32);
T u32 t_fnptr(u32 a, u32 b, u32 c, u32 d) {
    static const op_fn ops[4] = { t_add, t_logic, t_switch, t_minmax };
    return ops[a & 3](b, c, d, a);
}
T u32 t_state(u32 a, u32 b, u32 c, u32 d) {
    /* xorshift RNG kept in a global, like the game's RNG */
    u32 x = g_state ^ a;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_state = x + b;
    return x ^ c ^ (d >> 1) ^ g_state;
}
T u32 t_sat(u32 a, u32 b, u32 c, u32 d) {
    s64 s = (s64)(s32)a + (s32)b;
    s32 r = s > 0x7fffffff ? 0x7fffffff : s < -0x7fffffffLL - 1 ? (s32)0x80000000 : (s32)s;
    s32 h = (s32)(int16_t)c * (s32)(int16_t)(d >> 16);
    return (u32)r ^ (u32)h;
}
T u32 t_bool_chain(u32 a, u32 b, u32 c, u32 d) {
    return (a && b) + 2 * (c || !d) + 4 * ((s32)a > (s32)c && b < d) + 8 * (a == (b ^ c));
}

/* Integer division helpers for the ARM build (the native build doesn't use them). */
#ifdef __arm__
u32 __aeabi_uidiv(u32 n, u32 d) {
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1u << i; } }
    return q;
}
s32 __aeabi_idiv(s32 n, s32 d) {
    u32 un = n < 0 ? -(u32)n : (u32)n, ud = d < 0 ? -(u32)d : (u32)d;
    u32 q = __aeabi_uidiv(un, ud);
    return (n < 0) != (d < 0) ? -(s32)q : (s32)q;
}
__asm__(
    ".global __aeabi_uidivmod\n.type __aeabi_uidivmod,%function\n.arm\n"
    "__aeabi_uidivmod:\n push {r0, r1, r4, lr}\n bl __aeabi_uidiv\n pop {r1, r2}\n"
    " mul r3, r0, r2\n sub r1, r1, r3\n pop {r4, pc}\n.size __aeabi_uidivmod, .-__aeabi_uidivmod\n"
    ".global __aeabi_idivmod\n.type __aeabi_idivmod,%function\n.arm\n"
    "__aeabi_idivmod:\n push {r0, r1, r4, lr}\n bl __aeabi_idiv\n pop {r1, r2}\n"
    " mul r3, r0, r2\n sub r1, r1, r3\n pop {r4, pc}\n.size __aeabi_idivmod, .-__aeabi_idivmod\n");
/* Thumb-1 has no 64-bit multiply: clang calls this. Written in ARM so Thumb
 * callers also exercise ARM<->Thumb interworking (blx / bx lr). */
__asm__(
    ".global __aeabi_lmul\n.type __aeabi_lmul,%function\n.arm\n.p2align 2\n"
    "__aeabi_lmul:\n mul r3, r0, r3\n mla r3, r2, r1, r3\n umull r0, r1, r2, r0\n add r1, r1, r3\n bx lr\n"
    ".size __aeabi_lmul, .-__aeabi_lmul\n");
int __clzsi2(u32 x) { int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
void* memcpy(void* d, const void* s, unsigned n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int v, unsigned n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)v; return d; }
#endif

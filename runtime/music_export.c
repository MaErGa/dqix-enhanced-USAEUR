/* Exporting the game's songs as WAV files (Mods > Export the game's songs),
 * so music pack makers can hear what each BG_/ME_ name is and line their
 * recordings up with its loop.
 *
 * The songs are sequences (notes) played with instruments (SBNK) made of
 * samples (SWAR), all in the ROM's sound archive. This is a sequence player
 * of its own, after the Nitro SDK's sound driver as it is documented by the
 * DS homebrew and ripping community (envelopes, decibel volumes, note-wait,
 * variables, loops): close to the game's sound, not sample-exact. It reads
 * only the ROM, never the running game, so it can run on its own thread.
 *
 * Each file holds the intro and one pass of the loop, with the loop's start
 * and end as a sampler loop ("smpl" chunk) and in songs.txt; music packs read
 * those loop points from WAV files, so an exported (or edited) file can go
 * straight back into the music folder. Jingles (no loop) end with their last
 * notes' release. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "nds.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE 32728                         /* the DS's output rate */
#define FRAME_HZ 192.0                     /* the sound driver's update rate (64 * 2728 cycles at 33.5 MHz) */
#define MAXV 32                            /* voices at once (the DS has 16 channels; a little room) */
#define MAX_SECONDS 600

typedef struct { const uint8_t* p; uint32_t n; } Blob;

/* ---------------------------------------------------------------- the archive */
static const uint8_t* g_sd; static uint32_t g_sdn;
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t sd32(uint32_t o) { return o + 4 <= g_sdn ? rd32(g_sd + o) : 0; }
static uint16_t sd16(uint32_t o) { return o + 2 <= g_sdn ? rd16(g_sd + o) : 0; }

static uint32_t rec_n(int k) { uint32_t info = sd32(0x18), r = info + sd32(info + 8 + 4 * (uint32_t)k); return sd32(r); }
static uint32_t rec_at(int k, uint32_t i) { uint32_t info = sd32(0x18), r = info + sd32(info + 8 + 4 * (uint32_t)k); uint32_t p = sd32(r + 4 + 4 * i); return p ? info + p : 0; }
static Blob sd_file(uint32_t fid) {
    Blob b = { 0, 0 }; uint32_t fat = sd32(0x20);
    if (fid >= sd32(fat + 8)) return b;
    uint32_t o = sd32(fat + 12 + 16 * fid), l = sd32(fat + 16 + 16 * fid);
    if (o + l > g_sdn) return b;
    b.p = g_sd + o; b.n = l; return b;
}
static int find_sdat(void) {
    const uint8_t magic[8] = { 'S', 'D', 'A', 'T', 0xFF, 0xFE, 0x00, 0x01 };
    for (size_t at = 0; at + 0x40 < g_rom_size; at += 0x200) {
        if (memcmp(g_rom + at, magic, 8)) continue;
        uint32_t n = rd32(g_rom + at + 8);
        if (n < 0x40 || at + n > g_rom_size) continue;
        g_sd = g_rom + at; g_sdn = n;
        if (rec_n(0) > 0 && rec_n(0) < 4096) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- samples */
typedef struct { float* s; uint32_t n, loop; int looped; double rate; } Wave;   /* loop: start sample (if looped) */
typedef struct { int swar_id; Wave* w; uint32_t n; } WaveArc;
static WaveArc g_wa[16]; static int g_nwa;

static void wave_decode(const uint8_t* e, uint32_t avail, Wave* w) {
    memset(w, 0, sizeof *w);
    if (avail < 12) return;
    int type = e[0]; w->looped = e[1]; w->rate = rd16(e + 2) ? rd16(e + 2) : 32728;
    uint32_t ls = rd16(e + 6), ll = rd32(e + 8);
    const uint8_t* d = e + 12; uint32_t bytes = (ls + ll) * 4;
    if (bytes > avail - 12) bytes = avail - 12;
    if (type == 0) {                                          /* PCM8 */
        w->n = bytes; w->s = malloc(sizeof(float) * (w->n + 1));
        for (uint32_t i = 0; i < w->n; i++) w->s[i] = (float)(int8_t)d[i] / 128.0f;
        w->loop = ls * 4;
    } else if (type == 1) {                                   /* PCM16 */
        w->n = bytes / 2; w->s = malloc(sizeof(float) * (w->n + 1));
        for (uint32_t i = 0; i < w->n; i++) w->s[i] = (float)(int16_t)rd16(d + 2 * i) / 32768.0f;
        w->loop = ls * 2;
    } else if (type == 2) {                                   /* IMA-ADPCM: a 4-byte header, then nibbles */
        static const int8_t it[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
        static const uint16_t st[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
            130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
            1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
            13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
        if (bytes < 4) return;
        int val = (int16_t)rd16(d), idx = d[2] > 88 ? 88 : d[2];
        w->n = (bytes - 4) * 2; w->s = malloc(sizeof(float) * (w->n + 1));
        for (uint32_t i = 0; i < w->n; i++) {
            int nib = (d[4 + i / 2] >> ((i & 1) * 4)) & 15, step = st[idx], diff = step >> 3;
            if (nib & 1) diff += step >> 2; if (nib & 2) diff += step >> 1; if (nib & 4) diff += step;
            val += (nib & 8) ? -diff : diff;
            if (val > 32767) val = 32767; if (val < -32768) val = -32768;
            idx += it[nib & 7]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
            w->s[i] = (float)val / 32768.0f;
        }
        w->loop = ls * 4 >= 4 ? (ls * 4 - 4) * 2 : 0;
    }
    if (w->s) w->s[w->n] = w->n ? w->s[w->looped ? w->loop : w->n - 1] : 0;   /* one past the end, for interpolation */
    if (w->loop >= w->n) w->looped = 0;
}

static WaveArc* wave_arc(int id) {
    for (int i = 0; i < g_nwa; i++) if (g_wa[i].swar_id == id) return &g_wa[i];
    if (g_nwa == 16) return NULL;
    uint32_t r = rec_at(3, (uint32_t)id);
    if (!r) return NULL;
    Blob b = sd_file(sd32(r) & 0xFFFFFF);
    if (!b.p || b.n < 0x3C || memcmp(b.p, "SWAR", 4)) return NULL;
    WaveArc* a = &g_wa[g_nwa++]; memset(a, 0, sizeof *a); a->swar_id = id;
    uint32_t n = rd32(b.p + 0x38);
    if (n > 4096 || 0x3C + n * 4 > b.n) return a;
    a->w = calloc(n, sizeof(Wave)); a->n = n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = rd32(b.p + 0x3C + 4 * i), next = i + 1 < n ? rd32(b.p + 0x3C + 4 * (i + 1)) : b.n;
        if (o < b.n && next > o && next <= b.n) wave_decode(b.p + o, next - o, &a->w[i]);
    }
    return a;
}
static void waves_free(void) {
    for (int i = 0; i < g_nwa; i++) { for (uint32_t k = 0; k < g_wa[i].n; k++) free(g_wa[i].w[k].s); free(g_wa[i].w); }
    g_nwa = 0;
}

/* ---------------------------------------------------------------- instruments */
typedef struct { int type; int swav, swar; int base, a, d, s, r, pan; } Inst;   /* type 1 PCM, 2 PSG, 3 noise */
typedef struct { Blob b; uint16_t wa[4]; } Bank;

static int read_inst(const uint8_t* p, int type, Inst* o) {
    memset(o, 0, sizeof *o);
    o->type = type; o->swav = rd16(p); o->swar = rd16(p + 2);
    o->base = p[4]; o->a = p[5]; o->d = p[6]; o->s = p[7]; o->r = p[8]; o->pan = p[9];
    return type >= 1 && type <= 4;
}
/* the instrument for program prg at key: plain, a drum set (one per key) or split by key ranges */
static int bank_inst(const Bank* bk, int prg, int key, Inst* o) {
    const uint8_t* b = bk->b.p; uint32_t n = bk->b.n;
    if (!b || n < 0x3C || prg < 0 || (uint32_t)prg >= rd32(b + 0x38) || 0x3C + 4u * (uint32_t)prg + 4 > n) return 0;
    int type = b[0x3C + 4 * prg]; uint32_t off = rd16(b + 0x3C + 4 * prg + 1);
    if (!type || off + 12 > n) return 0;
    if (type < 16) return read_inst(b + off, type, o);
    if (type == 16) {                                         /* drum set: low, high, then (u16 type, 10 bytes) per key */
        int lo = b[off], hi = b[off + 1];
        if (key < lo || key > hi) return 0;
        uint32_t e = off + 2 + 12u * (uint32_t)(key - lo);
        if (e + 12 > n) return 0;
        return read_inst(b + e + 2, rd16(b + e), o);
    }
    if (type == 17) {                                         /* key split: 8 upper keys, then (u16 type, 10 bytes) per region */
        int r = 0;
        while (r < 8 && b[off + r] && key > b[off + r]) r++;
        if (r == 8 || !b[off + r]) return 0;
        uint32_t e = off + 8 + 12u * (uint32_t)r;
        if (e + 12 > n) return 0;
        return read_inst(b + e + 2, rd16(b + e), o);
    }
    return 0;
}

/* ---------------------------------------------------------------- volume (1/10 dB) and envelopes */
static int dbsq(int x) {                                      /* the SDK's square table: 0..127 -> -72.3..0 dB */
    if (x <= 0) return -723; if (x >= 127) return 0;
    int v = (int)lround(400.0 * log10((double)x / 127.0));
    return v < -723 ? -723 : v;
}
static int att_rate(int a) {
    static const uint8_t t[19] = { 0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49, 0x54, 0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F };
    return a >= 109 ? t[127 - a] : 255 - a;
}
static int fall_rate(int d) { if (d >= 127) return 0xFFFF; if (d == 126) return 0x3C00; if (d < 50) return d * 2 + 1; return 0x1E00 / (126 - d); }
#define ENV_MIN (-92544)

/* ---------------------------------------------------------------- voices */
typedef struct {
    int on, track, key, vel, released, env_state;             /* env_state 0 attack, 1 decay, 2 sustain, 3 release */
    int env, att, dec, sus, rel;
    int len;                                                  /* frames until key off (-1: held, a tie) */
    Inst in; const Wave* w; double pos, step0; int psg_phase; uint16_t lfsr;
    double ppos; int age;
} Voice;

typedef struct {
    int on, pos, wait, prg, vol, expr, pan, transpose, bend, range, mono, tie, prio;
    int a, d, s, r;                                           /* -1: the instrument's own */
    int mod_depth, mod_speed, mod_type, mod_range, mod_delay, lfo_wait; double lfo;
    int call[3], ncall, loop_pos[3], loop_n[3], nloop, cond;
    int tie_voice;
} Track;

typedef struct {
    const uint8_t* d; uint32_t n;                             /* the sequence's data */
    Bank bank; int vol;
    Track t[16]; Voice v[MAXV];
    int tempo, tcount; int16_t var[16];
    uint32_t rng;
    double tick_frames;                                       /* frames so far */
    /* loop: the first visit (frame) of every byte of the data, and the loop found */
    int32_t* first; int loop_found; double loop_start_f, loop_end_f; int ended;
} Seq;
static int16_t g_gvar[16];

static uint32_t varlen(Seq* q, int* pos) {
    uint32_t v = 0;
    for (int k = 0; k < 4 && (uint32_t)*pos < q->n; k++) { uint8_t b = q->d[(*pos)++]; v = (v << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
    return v;
}
static int u8a(Seq* q, int* pos) { return (uint32_t)*pos < q->n ? q->d[(*pos)++] : 0; }
static int s16a(Seq* q, int* pos) { int v = (uint32_t)*pos + 1 < q->n ? (int16_t)rd16(q->d + *pos) : 0; *pos += 2; return v; }
static int u24a(Seq* q, int* pos) { int v = (uint32_t)*pos + 2 < q->n ? (q->d[*pos] | q->d[*pos + 1] << 8 | q->d[*pos + 2] << 16) : 0; *pos += 3; return v; }
static int rnd(Seq* q) { q->rng = q->rng * 1664525u + 1013904223u; return (int)(q->rng >> 16); }
static int16_t* var_p(Seq* q, int i) { return i < 16 ? &q->var[i] : &g_gvar[(i - 16) & 15]; }

/* an argument: plain, random (A0: s16 min, max) or a variable (A1: u8) */
static int arg(Seq* q, int* pos, int pre, int kind) {         /* kind 0 u8, 1 s8, 2 s16, 3 varlen, 4 u16 */
    if (pre == 0xA0) { int lo = s16a(q, pos), hi = s16a(q, pos); return hi > lo ? lo + rnd(q) % (hi - lo + 1) : lo; }
    if (pre == 0xA1) { int i = u8a(q, pos); return *var_p(q, i); }
    switch (kind) {
    case 0: return u8a(q, pos);
    case 1: return (int8_t)u8a(q, pos);
    case 2: return s16a(q, pos);
    case 4: { int v = (uint32_t)*pos + 1 < q->n ? rd16(q->d + *pos) : 0; *pos += 2; return v; }
    default: return (int)varlen(q, pos);
    }
}

static void voice_key_off(Voice* v) { if (v->on && v->env_state != 3) v->env_state = 3; }

static void note_on(Seq* q, int ti, int key, int vel, int len) {
    Track* t = &q->t[ti];
    int k = key + t->transpose; if (k < 0) k = 0; if (k > 127) k = 127;
    if (t->tie && t->tie_voice >= 0 && q->v[t->tie_voice].on && q->v[t->tie_voice].track == ti && q->v[t->tie_voice].env_state != 3) {
        Voice* v = &q->v[t->tie_voice]; v->key = k; v->vel = vel; v->len = len; return;   /* tie: the same note glides on */
    }
    Inst in;
    if (!bank_inst(&q->bank, t->prg, k, &in)) return;
    const Wave* w = NULL;
    if (in.type == 1 || in.type == 4) {
        if (in.swar > 3 || q->bank.wa[in.swar] == 0xFFFF) return;
        WaveArc* a = wave_arc(q->bank.wa[in.swar]);
        if (!a || (uint32_t)in.swav >= a->n || !a->w[in.swav].s || !a->w[in.swav].n) return;
        w = &a->w[in.swav];
    }
    int slot = -1, oldest = -1;
    for (int i = 0; i < MAXV; i++) { if (!q->v[i].on) { slot = i; break; } if (oldest < 0 || q->v[i].age > q->v[oldest].age) oldest = i; }
    if (slot < 0) slot = oldest;
    Voice* v = &q->v[slot]; memset(v, 0, sizeof *v);
    v->on = 1; v->track = ti; v->key = k; v->vel = vel; v->len = len; v->in = in; v->w = w; v->lfsr = 0x7FFF;
    v->att = att_rate(t->a >= 0 ? t->a : in.a); v->dec = fall_rate(t->d >= 0 ? t->d : in.d);
    v->sus = dbsq(t->s >= 0 ? t->s : in.s) << 7; v->rel = fall_rate(t->r >= 0 ? t->r : in.r);
    v->env = ENV_MIN; v->env_state = 0;
    if (t->tie) t->tie_voice = slot;
}

/* one tick of one track: commands until it waits */
static void track_tick(Seq* q, int ti) {
    Track* t = &q->t[ti];
    if (!t->on) return;
    if (t->wait > 0) { t->wait--; if (t->wait > 0) return; }
    for (int guard = 0; guard < 10000 && t->on && t->wait == 0; guard++) {
        if ((uint32_t)t->pos >= q->n) { t->on = 0; break; }
        if (q->first && q->first[t->pos] < 0) q->first[t->pos] = (int32_t)q->tick_frames;
        int pos = t->pos, c = q->d[pos++], pre = 0;
        int run = 1;
        if (c == 0xA2) { run = t->cond; c = q->d[pos++]; }             /* if: the next command runs only when the last compare was true */
        if (c == 0xA0 || c == 0xA1) { pre = c; c = q->d[pos++]; }
        if (c < 0x80) {
            int vel = u8a(q, &pos), len = arg(q, &pos, pre, 3);
            if (run) { note_on(q, ti, c, vel, len ? len : -1); if (t->mono) t->wait = len; }
        } else switch (c) {
        case 0x80: { int w = arg(q, &pos, pre, 3); if (run) t->wait = w; break; }
        case 0x81: { int p = arg(q, &pos, pre, 3); if (run) t->prg = p; break; }
        case 0x93: { int tr = u8a(q, &pos), off = u24a(q, &pos); if (run && tr < 16 && !q->t[tr].on) { Track* n = &q->t[tr]; int keep = n->tie_voice; memset(n, 0, sizeof *n); (void)keep;
                       n->on = 1; n->pos = off; n->vol = 127; n->expr = 127; n->pan = 64; n->range = 2; n->mono = 1; n->a = n->d = n->s = n->r = -1; n->mod_speed = 16; n->mod_range = 1; n->tie_voice = -1; } break; }
        case 0x94: { int off = u24a(q, &pos); if (run) {
                       if (off < t->pos && !q->loop_found && q->first && q->first[off] >= 0) { q->loop_found = 1; q->loop_start_f = q->first[off]; q->loop_end_f = q->tick_frames; }
                       pos = off; } break; }
        case 0x95: { int off = u24a(q, &pos); if (run && t->ncall < 3) { t->call[t->ncall++] = pos; pos = off; } break; }
        case 0xFD: if (run && t->ncall) pos = t->call[--t->ncall]; break;
        case 0xD4: { int n = arg(q, &pos, pre, 0); if (run && t->nloop < 3) { t->loop_pos[t->nloop] = pos; t->loop_n[t->nloop] = n; t->nloop++; } break; }
        case 0xFC: if (run && t->nloop) {
                       int k = t->nloop - 1;
                       if (t->loop_n[k] == 0) {                       /* forever: the song's loop */
                           if (!q->loop_found && q->first && q->first[t->loop_pos[k]] >= 0) { q->loop_found = 1; q->loop_start_f = q->first[t->loop_pos[k]]; q->loop_end_f = q->tick_frames; }
                           pos = t->loop_pos[k];
                       } else if (--t->loop_n[k] > 0) pos = t->loop_pos[k]; else t->nloop--;
                   } break;
        case 0xFE: pos += 2; break;
        case 0xFF: if (run) { t->on = 0; } break;
        case 0xC0: { int v = arg(q, &pos, pre, 0); if (run) t->pan = v; break; }
        case 0xC1: { int v = arg(q, &pos, pre, 0); if (run) t->vol = v; break; }
        case 0xC2: { int v = arg(q, &pos, pre, 0); if (run) q->vol = v; break; }
        case 0xC3: { int v = arg(q, &pos, pre, 1); if (run) t->transpose = v; break; }
        case 0xC4: { int v = arg(q, &pos, pre, 1); if (run) t->bend = v; break; }
        case 0xC5: { int v = arg(q, &pos, pre, 0); if (run) t->range = v; break; }
        case 0xC6: { int v = arg(q, &pos, pre, 0); if (run) t->prio = v; break; }
        case 0xC7: { int v = arg(q, &pos, pre, 0); if (run) t->mono = v != 0; break; }
        case 0xC8: { int v = arg(q, &pos, pre, 0); if (run) { t->tie = v != 0; if (!t->tie && t->tie_voice >= 0) voice_key_off(&q->v[t->tie_voice]); t->tie_voice = -1; } break; }
        case 0xCA: { int v = arg(q, &pos, pre, 0); if (run) t->mod_depth = v; break; }
        case 0xCB: { int v = arg(q, &pos, pre, 0); if (run) t->mod_speed = v; break; }
        case 0xCC: { int v = arg(q, &pos, pre, 0); if (run) t->mod_type = v; break; }
        case 0xCD: { int v = arg(q, &pos, pre, 0); if (run) t->mod_range = v; break; }
        case 0xD0: { int v = arg(q, &pos, pre, 0); if (run) t->a = v; break; }
        case 0xD1: { int v = arg(q, &pos, pre, 0); if (run) t->d = v; break; }
        case 0xD2: { int v = arg(q, &pos, pre, 0); if (run) t->s = v; break; }
        case 0xD3: { int v = arg(q, &pos, pre, 0); if (run) t->r = v; break; }
        case 0xD5: { int v = arg(q, &pos, pre, 0); if (run) t->expr = v; break; }
        case 0xC9: case 0xCE: case 0xCF: case 0xD6: case 0xD7: arg(q, &pos, pre, 0); break;   /* portamento, print: not here */
        case 0xE0: { int v = arg(q, &pos, pre, 2); if (run) t->mod_delay = v; break; }
        case 0xE1: { int v = arg(q, &pos, pre, 4); if (run) q->tempo = v; break; }
        case 0xE3: arg(q, &pos, pre, 2); break;                         /* sweep: not here */
        default:
            if (c >= 0xB0 && c <= 0xBD) {
                int i = u8a(q, &pos), val = arg(q, &pos, pre, 2);
                if (!run) break;
                int16_t* x = var_p(q, i);
                switch (c) {
                case 0xB0: *x = (int16_t)val; break; case 0xB1: *x += (int16_t)val; break; case 0xB2: *x -= (int16_t)val; break;
                case 0xB3: *x = (int16_t)(*x * val); break; case 0xB4: if (val) *x = (int16_t)(*x / val); break;
                case 0xB5: *x = (int16_t)(val >= 0 ? *x << val : *x >> -val); break;
                case 0xB6: *x = (int16_t)(val ? rnd(q) % (abs(val) + 1) * (val < 0 ? -1 : 1) : 0); break;
                case 0xB8: t->cond = *x == val; break; case 0xB9: t->cond = *x >= val; break; case 0xBA: t->cond = *x > val; break;
                case 0xBB: t->cond = *x <= val; break; case 0xBC: t->cond = *x < val; break; case 0xBD: t->cond = *x != val; break;
                }
            } else t->on = 0;                                           /* unknown: stop the track rather than guess */
        }
        t->pos = pos;
    }
}

/* one driver frame (1/192 s): the tempo's ticks, then the envelopes and note lengths */
static void seq_frame(Seq* q) {
    q->tcount += q->tempo;
    while (q->tcount >= 240) {
        q->tcount -= 240;
        for (int i = 0; i < MAXV; i++) {                            /* note lengths run in ticks */
            Voice* v = &q->v[i];
            if (v->on && v->len > 0 && v->env_state != 3 && --v->len == 0) voice_key_off(v);
        }
        for (int ti = 0; ti < 16; ti++) track_tick(q, ti);
    }
    for (int i = 0; i < MAXV; i++) {
        Voice* v = &q->v[i];
        if (!v->on) continue;
        v->age++;
        switch (v->env_state) {
        case 0: v->env = -(int)(((int64_t)-v->env * v->att) >> 8); if (v->env >= 0 || v->att == 0) { v->env = 0; v->env_state = 1; } break;
        case 1: v->env -= v->dec; if (v->env <= v->sus) { v->env = v->sus; v->env_state = 2; } break;
        case 2: break;
        case 3: v->env -= v->rel; if (v->env <= ENV_MIN) v->on = 0; break;
        }
    }
    for (int ti = 0; ti < 16; ti++) {                               /* modulation (vibrato and the like) */
        Track* t = &q->t[ti];
        if (!t->on || !t->mod_depth) continue;
        if (t->lfo_wait < t->mod_delay) { t->lfo_wait++; continue; }
        t->lfo += (double)t->mod_speed * 64.0 / 32768.0;
        if (t->lfo >= 1.0) t->lfo -= floor(t->lfo);
    }
    q->tick_frames += 1.0;
}

/* the voices' samples for one output sample */
static void seq_sample(Seq* q, float* l, float* r) {
    float sl = 0, sr = 0;
    for (int i = 0; i < MAXV; i++) {
        Voice* v = &q->v[i];
        if (!v->on) continue;
        Track* t = &q->t[v->track];
        double lfo = t->mod_depth && t->lfo_wait >= t->mod_delay ? sin(t->lfo * 2 * M_PI) * t->mod_depth * t->mod_range / 128.0 : 0;
        double semis = v->key - v->in.base + t->bend * t->range / 128.0 + (t->mod_type == 0 ? lfo : 0);
        double s;
        if (v->w) {
            double step = v->w->rate / RATE * pow(2.0, semis / 12.0);
            uint32_t ip = (uint32_t)v->pos; double f = v->pos - ip;
            if (ip >= v->w->n) { v->on = 0; continue; }
            s = v->w->s[ip] + (v->w->s[ip + 1] - v->w->s[ip]) * f;
            v->pos += step;
            if (v->pos >= v->w->n) { if (v->w->looped && v->w->n > v->w->loop) v->pos = v->w->loop + fmod(v->pos - v->w->n, (double)(v->w->n - v->w->loop)); else v->on = 0; }
        } else {
            double hz = 261.63 * pow(2.0, semis / 12.0);
            if (v->in.type == 3) {                                  /* noise */
                v->ppos += hz * 8.0 / RATE;
                while (v->ppos >= 1.0) { v->ppos -= 1.0; int b = v->lfsr & 1; v->lfsr >>= 1; if (b) v->lfsr ^= 0x6000; }
                s = (v->lfsr & 1) ? 0.5 : -0.5;
            } else {                                                /* PSG: a square wave, duty (n + 1) / 8 */
                v->ppos += hz / RATE; v->ppos -= floor(v->ppos);
                int duty = v->in.swav & 7;
                s = duty == 7 ? -0.5 : (v->ppos * 8.0 < duty + 1 ? 0.5 : -0.5);
            }
        }
        int db = (v->env >> 7) + dbsq(v->vel) + dbsq(t->vol) + dbsq(t->expr) + dbsq(q->vol);
        if (t->mod_type == 1) db += (int)(lfo * 60.0);
        if (db <= -723) continue;
        float amp = (float)pow(10.0, db / 200.0);
        int pan = t->pan + v->in.pan - 64; if (t->mod_type == 2) pan += (int)(lfo * 32);
        if (pan < 0) pan = 0; if (pan > 127) pan = 127;
        sl += (float)s * amp * (float)(127 - pan) / 127.0f;
        sr += (float)s * amp * (float)pan / 127.0f;
    }
    *l = sl; *r = sr;
}

static int voices_on(Seq* q) { for (int i = 0; i < MAXV; i++) if (q->v[i].on) return 1; return 0; }
static int tracks_on(Seq* q) { for (int i = 0; i < 16; i++) if (q->t[i].on) return 1; return 0; }

/* ---------------------------------------------------------------- WAV out */
static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static int wav_write(const char* path, const int16_t* pcm, uint32_t frames, int looped, uint32_t ls, uint32_t le) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t data = frames * 4, smpl = looped ? 36 + 24 : 0;
    fwrite("RIFF", 1, 4, f); put32(f, 4 + 24 + 8 + data + (smpl ? 8 + smpl : 0)); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2); put32(f, RATE); put32(f, RATE * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data); fwrite(pcm, 4, frames, f);
    if (smpl) {                                                     /* the loop, as samplers and editors read it (end: the last sample in it) */
        fwrite("smpl", 1, 4, f); put32(f, smpl);
        put32(f, 0); put32(f, 0); put32(f, (uint32_t)(1000000000.0 / RATE)); put32(f, 60); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 1); put32(f, 0);
        put32(f, 0); put32(f, 0); put32(f, ls); put32(f, le - 1); put32(f, 0); put32(f, 0);
    }
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------- one song */
typedef struct { char name[32]; double seconds, loop_start, loop_len; int looped, ok; } SongOut;

static int render_song(uint32_t k, const char* name, const char* dir, SongOut* out) {
    memset(out, 0, sizeof *out); snprintf(out->name, sizeof out->name, "%s", name);
    uint32_t si = rec_at(0, k);
    if (!si) return 0;
    Blob sq = sd_file(sd32(si) & 0xFFFFFF);
    if (!sq.p || sq.n < 0x20 || memcmp(sq.p, "SSEQ", 4)) return 0;
    uint32_t doff = rd32(sq.p + 0x18);
    if (doff >= sq.n) return 0;
    Seq* q = calloc(1, sizeof *q);
    q->d = sq.p + doff; q->n = sq.n - doff;
    q->vol = g_sd[si + 6]; q->tempo = 120; q->rng = 0x12345u + k;
    for (int i = 0; i < 16; i++) q->var[i] = -1;
    uint32_t bi = rec_at(2, sd16(si + 4));
    if (bi) { q->bank.b = sd_file(sd32(bi) & 0xFFFFFF); for (int i = 0; i < 4; i++) q->bank.wa[i] = sd16(bi + 4 + 2 * (uint32_t)i); }
    q->first = malloc(sizeof(int32_t) * q->n); for (uint32_t i = 0; i < q->n; i++) q->first[i] = -1;
    Track* t0 = &q->t[0]; t0->on = 1; t0->vol = 127; t0->expr = 127; t0->pan = 64; t0->range = 2; t0->mono = 1;
    t0->a = t0->d = t0->s = t0->r = -1; t0->mod_speed = 16; t0->mod_range = 1; t0->tie_voice = -1;
    for (int i = 1; i < 16; i++) q->t[i].tie_voice = -1;

    uint32_t cap = RATE * MAX_SECONDS, nf = 0;
    float* buf = malloc(sizeof(float) * 2 * (size_t)cap);
    double per_frame = RATE / FRAME_HZ, acc = 0;
    uint32_t end_at = 0, tail = 0;
    while (nf < cap && buf) {
        seq_frame(q);
        if (q->loop_found) { end_at = (uint32_t)lround(q->loop_end_f * per_frame); break; }
        acc += per_frame;
        while (acc >= 1.0 && nf < cap) { seq_sample(q, &buf[nf * 2], &buf[nf * 2 + 1]); nf++; acc -= 1.0; }
        if (!tracks_on(q)) { if (!voices_on(q) || ++tail > FRAME_HZ * 8) break; }
    }
    int looped = q->loop_found;
    uint32_t ls = looped ? (uint32_t)lround(q->loop_start_f * per_frame) : 0, le = looped ? end_at : nf;
    if (looped && le > nf) le = nf;
    if (looped && ls >= le) looped = 0, le = nf;
    /* the level: the loudest sample at -1 dB (the game mixes songs lower; a pack's level is music_pack_volume) */
    float peak = 1e-6f;
    for (uint32_t i = 0; i < le * 2; i++) { float a = fabsf(buf[i]); if (a > peak) peak = a; }
    float g = 0.89f / peak;
    int16_t* pcm = malloc((size_t)le * 4 + 4);
    for (uint32_t i = 0; pcm && i < le * 2; i++) { float x = buf[i] * g * 32767.0f; pcm[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
    char path[600]; snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    int ok = pcm && le > 0 && wav_write(path, pcm, le, looped, ls, le);
    out->ok = ok; out->looped = looped; out->seconds = (double)le / RATE;
    out->loop_start = (double)ls / RATE; out->loop_len = (double)(le - ls) / RATE;
    free(pcm); free(buf); free(q->first); free(q);
    return ok;
}

/* All songs to dir (it must exist); progress(done, total) after each. Returns how many were written. */
int music_export(const char* dir, void (*progress)(int, int)) {
    if (!g_rom || !find_sdat()) return 0;
    uint32_t nseq = rec_n(0), symb = sd32(0x10), srec = symb ? symb + sd32(symb + 8) : 0, nsym = srec ? sd32(srec) : 0;
    char list[600]; snprintf(list, sizeof list, "%s/loops.txt", dir);
    FILE* lf = fopen(list, "w");
    if (lf) fprintf(lf, "# The game's songs, made from the ROM's sequences (close to the game, not exact).\n"
                        "# Each file: the intro and one pass of the loop; the loop is also in the file (a sampler loop).\n"
                        "# name      length   loop start  loop length (seconds; in samples at %d Hz)\n", RATE);
    int done = 0, total = 0;
    for (uint32_t k = 0; k < nseq; k++) if (rec_at(0, k)) total++;
    for (uint32_t k = 0; k < nseq; k++) {
        if (!rec_at(0, k)) continue;
        char name[32] = "";
        if (k < nsym) { uint32_t np = sd32(srec + 4 + 4 * k); if (np && symb + np < g_sdn) { size_t l = 0; const char* s = (const char*)g_sd + symb + np;
            while (l < sizeof name - 1 && s[l] && symb + np + l < g_sdn) { char c = s[l]; name[l] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_'; l++; } name[l] = 0; } }
        if (!name[0]) snprintf(name, sizeof name, "SEQ_%03u", k);
        SongOut o;
        if (render_song(k, name, dir, &o) && lf) {
            if (o.looped) fprintf(lf, "%-10s %6.1f s  %8.2f s  %8.2f s   LOOPSTART=%u LOOPLENGTH=%u\n", o.name, o.seconds, o.loop_start, o.loop_len,
                                  (unsigned)lround(o.loop_start * RATE), (unsigned)lround(o.loop_len * RATE));
            else fprintf(lf, "%-10s %6.1f s  (plays once)\n", o.name, o.seconds);
        }
        done++;
        if (progress) progress(done, total);
    }
    if (lf) fclose(lf);
    waves_free();
    return done;
}

/* ARM7 sound hardware: 16 channels (PCM8, PCM16, IMA-ADPCM, PSG, noise), the
 * two capture units (used for reverb), the mixer and its output routing,
 * producing samples at the DS rate (33.51 MHz / 1024 = ~32728 Hz) into a ring
 * buffer that the frontend plays.
 *
 * This follows the hardware's behaviour as worked out by the melonDS project
 * (SPU.cpp, GPL): each channel reads its data through a small FIFO, starts a
 * few samples late, loops / stops by the exact rules, and the capture units
 * write the mixed output back to memory through their own FIFO. Output is
 * reduced to 10 bits like the DS's DAC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"

typedef int32_t s32; typedef uint32_t u32; typedef int16_t s16; typedef uint16_t u16; typedef uint8_t u8; typedef int8_t s8;

#define IO7_32_(o) (*(uint32_t*)&g_io7[o])
#define IO7_16_(o) (*(uint16_t*)&g_io7[o])

static const s8 adpcm_index_tab[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const u16 adpcm_tab[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019, 0x001C, 0x001F,
    0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042, 0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F,
    0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0,
    0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954, 0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B,
    0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF };
static const s16 psg_tab[8][8] = {
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF } };

/* ---------------------------------------------------------------- channels */
typedef struct {
    u32 cnt;                   /* control (bit 31 = playing) */
    int keyon;
    u32 timer;
    s32 pos;
    s32 cur;
    u32 fifo[8];
    u32 frpos, fwpos, froff, flevel;   /* read position (bytes), write slot, source offset, level (bytes) */
    s32 adpcm_val, adpcm_idx, adpcm_val_loop, adpcm_idx_loop;
    u8 adpcm_byte;
    u16 noise;
} Chan;
static Chan g_ch[16];

static inline u32 ch_src(int n)  { return IO7_32_(0x404 + n * 16) & 0x07FFFFFC; }
static inline u32 ch_reload(int n) { return IO7_16_(0x408 + n * 16); }
static inline u32 ch_looppos(int n) { return (u32)IO7_16_(0x40A + n * 16) << 2; }
static inline u32 ch_length(int n) { return (IO7_32_(0x40C + n * 16) & 0x001FFFFF) << 2; }

static void fifo_buffer(int n) {
    Chan* c = &g_ch[n];
    u32 loop = ch_looppos(n), total = loop + ch_length(n);
    if (c->froff >= total) {
        u32 rep = (c->cnt >> 27) & 3;
        if (rep & 1) c->froff = loop;
        else if (rep & 2) return;                                   /* one-shot: done */
    }
    u32 burst = 16;
    if (c->froff + 16 > total) burst = total - c->froff;
    u32 src = ch_src(n);
    for (u32 i = 0; i < burst; i += 4) {
        u32 a = src + c->froff;
        c->fifo[c->fwpos] = a >= 0x4000 ? g_c7.bus->r32(a) : 0;       /* sound DMA can't read the ARM7 BIOS */
        c->froff += 4;
        c->fwpos = (c->fwpos + 1) & 7;
    }
    c->flevel += burst;
}
static u32 fifo_read(int n, int size) {
    Chan* c = &g_ch[n];
    const u8* b = (const u8*)c->fifo;
    u32 v;
    if (size == 1) v = b[c->frpos];
    else if (size == 2) v = (u32)b[c->frpos] | ((u32)b[(c->frpos + 1) & 31] << 8);
    else v = (u32)b[c->frpos] | ((u32)b[(c->frpos + 1) & 31] << 8) | ((u32)b[(c->frpos + 2) & 31] << 16) | ((u32)b[(c->frpos + 3) & 31] << 24);
    c->frpos = (c->frpos + (u32)size) & 31;
    c->flevel -= (u32)size;
    if (c->flevel <= 16) fifo_buffer(n);
    return v;
}

static void chan_start(int n) {
    Chan* c = &g_ch[n];
    c->timer = ch_reload(n);
    c->pos = ((c->cnt >> 29) & 3) == 3 ? -1 : -3;                  /* the first samples come out late */
    c->noise = 0x7FFF;
    c->cur = 0;
    c->frpos = c->fwpos = c->froff = c->flevel = 0;
    if (((c->cnt >> 29) & 3) != 3) { fifo_buffer(n); fifo_buffer(n); }
    static int slog = -1; if (slog < 0) slog = getenv("DQIX_SNDLOG") != NULL;
    if (slog) fprintf(stderr, "[f%llu] ch%d start fmt %u rep %u sad %08x tmr %04x pnt %x len %x cnt %08x\n", (unsigned long long)g_frame, n,
                      (c->cnt >> 29) & 3, (c->cnt >> 27) & 3, ch_src(n), ch_reload(n), ch_looppos(n) >> 2, ch_length(n) >> 2, c->cnt);
}

static void chan_end(Chan* c) { c->cur = 0; c->cnt &= ~(1u << 31); }

static void next_pcm8(int n) {
    Chan* c = &g_ch[n];
    if (++c->pos < 0) return;
    u32 loop = ch_looppos(n);
    if ((u32)c->pos >= loop + ch_length(n)) {
        u32 rep = (c->cnt >> 27) & 3;
        if (rep & 1) c->pos = (s32)loop;
        else if (rep & 2) { chan_end(c); return; }
    }
    c->cur = (s32)(int8_t)fifo_read(n, 1) << 8;
}
static void next_pcm16(int n) {
    Chan* c = &g_ch[n];
    if (++c->pos < 0) return;
    u32 loop = ch_looppos(n);
    if (((u32)c->pos << 1) >= loop + ch_length(n)) {
        u32 rep = (c->cnt >> 27) & 3;
        if (rep & 1) c->pos = (s32)(loop >> 1);
        else if (rep & 2) { chan_end(c); return; }
    }
    c->cur = (s16)fifo_read(n, 2);
}
static void next_adpcm(int n) {
    Chan* c = &g_ch[n];
    c->pos++;
    if (c->pos < 8) {
        if (c->pos == 0) {                                          /* header: initial value and step index */
            u32 h = fifo_read(n, 4);
            c->adpcm_val = (s16)(h & 0xFFFF);
            c->adpcm_idx = (h >> 16) & 0x7F; if (c->adpcm_idx > 88) c->adpcm_idx = 88;
            c->adpcm_val_loop = c->adpcm_val; c->adpcm_idx_loop = c->adpcm_idx;
        }
        return;
    }
    u32 loop = ch_looppos(n);
    if (((u32)c->pos >> 1) >= loop + ch_length(n)) {
        u32 rep = (c->cnt >> 27) & 3;
        if (rep & 1) {
            c->pos = (s32)(loop << 1);
            c->adpcm_val = c->adpcm_val_loop; c->adpcm_idx = c->adpcm_idx_loop;
            c->adpcm_byte = (u8)fifo_read(n, 1);
        } else if (rep & 2) { chan_end(c); return; }
    } else {
        if (!(c->pos & 1)) c->adpcm_byte = (u8)fifo_read(n, 1);
        else c->adpcm_byte >>= 4;
        u16 v = adpcm_tab[c->adpcm_idx];
        u16 diff = v >> 3;
        if (c->adpcm_byte & 1) diff += v >> 2;
        if (c->adpcm_byte & 2) diff += v >> 1;
        if (c->adpcm_byte & 4) diff += v;
        if (c->adpcm_byte & 8) { c->adpcm_val -= diff; if (c->adpcm_val < -0x7FFF) c->adpcm_val = -0x7FFF; }
        else { c->adpcm_val += diff; if (c->adpcm_val > 0x7FFF) c->adpcm_val = 0x7FFF; }
        c->adpcm_idx += adpcm_index_tab[c->adpcm_byte & 7];
        if (c->adpcm_idx < 0) c->adpcm_idx = 0; else if (c->adpcm_idx > 88) c->adpcm_idx = 88;
        if ((u32)c->pos == (loop << 1)) { c->adpcm_val_loop = c->adpcm_val; c->adpcm_idx_loop = c->adpcm_idx; }
    }
    c->cur = c->adpcm_val;
}

/* Run channel n for `cycles` (16.76 MHz) and return its output before panning. */
static s32 chan_run(int n, u32 cycles) {
    Chan* c = &g_ch[n];
    if (!(c->cnt & (1u << 31))) return 0;
    u32 type = (c->cnt >> 29) & 3;
    if (type == 3) { if (n >= 14) type = 4; else if (n < 8) return 0; }
    if (type < 3 && ch_looppos(n) + ch_length(n) < 16) return 0;
    if (c->keyon) { chan_start(n); c->keyon = 0; }
    c->timer += cycles;
    while (c->timer >> 16) {
        c->timer = ch_reload(n) + (c->timer - 0x10000);
        switch (type) {
        case 0: next_pcm8(n); break;
        case 1: next_pcm16(n); break;
        case 2: next_adpcm(n); break;
        case 3: c->pos++; c->cur = psg_tab[(c->cnt >> 24) & 7][c->pos & 7]; break;
        case 4:
            if (c->noise & 1) { c->noise = (u16)((c->noise >> 1) ^ 0x6000); c->cur = -0x7FFF; }
            else { c->noise >>= 1; c->cur = 0x7FFF; }
            break;
        }
        if (!(c->cnt & (1u << 31))) break;
    }
    static const u8 volshift[4] = { 4, 3, 2, 0 };
    u32 vol = c->cnt & 0x7F; if (vol == 127) vol = 128;
    return (c->cur << volshift[(c->cnt >> 8) & 3]) * (s32)vol;
}
static inline void pan_out(int n, s32 in, s32* l, s32* r) {
    u32 pan = (g_ch[n].cnt >> 16) & 0x7F; if (pan == 127) pan = 128;
    *l += (s32)(((int64_t)in * (128 - (s32)pan)) >> 10);
    *r += (s32)(((int64_t)in * (s32)pan) >> 10);
}

/* ---------------------------------------------------------------- capture units */
typedef struct {
    u8 cnt;
    u32 timer; s32 pos;
    u32 fifo[4];
    u32 frpos, fwpos, fwoff, flevel;
} Cap;
static Cap g_cap[2];
static inline u32 cap_dst(int i) { return IO7_32_(0x510 + i * 8) & 0x07FFFFFC; }
static inline u32 cap_len(int i) { u32 l = (u32)IO7_16_(0x514 + i * 8) << 2; return l ? l : 4; }
static inline u32 cap_reload(int i) { return ch_reload(i ? 3 : 1); }

static void cap_flush(int i) {
    Cap* c = &g_cap[i];
    u32 len = cap_len(i);
    for (int k = 0; k < 4; k++) {
        g_c7.bus->w32(cap_dst(i) + c->fwoff, c->fifo[c->frpos]);
        c->frpos = (c->frpos + 1) & 3;
        c->flevel -= 4;
        c->fwoff += 4;
        if (c->fwoff >= len) { c->fwoff = 0; break; }
    }
}
static void cap_write(int i, u32 v, int size) {
    Cap* c = &g_cap[i];
    u8* b = (u8*)c->fifo;
    for (int k = 0; k < size; k++) b[(c->fwpos + (u32)k) & 15] = (u8)(v >> (8 * k));
    c->fwpos = (c->fwpos + (u32)size) & 15;
    c->flevel += (u32)size;
    if (c->flevel >= 16) cap_flush(i);
}
static void cap_run(int i, u32 cycles, s32 sample) {
    Cap* c = &g_cap[i];
    c->timer += cycles;
    u32 len = cap_len(i);
    while (c->timer >> 16) {
        c->timer = cap_reload(i) + (c->timer - 0x10000);
        if (c->cnt & 0x08) { cap_write(i, (u32)(u8)(int8_t)(sample >> 8), 1); c->pos++; }
        else { cap_write(i, (u32)(u16)(s16)sample, 2); c->pos += 2; }
        if ((u32)c->pos >= len) {
            if (c->flevel >= 4) cap_flush(i);
            if (c->cnt & 0x04) { c->cnt &= 0x7F; g_io7[0x508 + i] = c->cnt; return; }   /* one-shot: stop */
            c->pos = 0;
        }
    }
}
/* SOUNDCAP0/1CNT written (0x04000508/9). */
void sound_capture_written(int i, u8 v) {
    Cap* c = &g_cap[i];
    if ((v & 0x80) && !(c->cnt & 0x80)) {
        c->timer = cap_reload(i); c->pos = 0;
        c->frpos = c->fwpos = c->fwoff = c->flevel = 0;
    }
    v &= 0x8F;
    if (!(v & 0x80)) v &= (u8)~0x01;
    c->cnt = v;
    g_io7[0x508 + i] = v;
}

/* ---------------------------------------------------------------- mixer */
static u32 g_mix_cycles;
#define OUT_N 16384                    /* stereo frames in the ring */
static int16_t g_out[OUT_N * 2];
static volatile uint32_t g_out_w, g_out_r;

static void mix_one(void) {
    s32 left = 0, right = 0, lo = 0, ro = 0;
    u32 cnt = IO7_32_(0x500);
    const u32 cyc = 512;                                            /* 1024 bus cycles = 512 channel-timer ticks */
    if (cnt & (1u << 15)) {
        s32 ch0 = chan_run(0, cyc), ch1 = chan_run(1, cyc), ch2 = chan_run(2, cyc), ch3 = chan_run(3, cyc);
        pan_out(0, ch0, &left, &right);
        pan_out(2, ch2, &left, &right);
        if (!(cnt & (1u << 12))) pan_out(1, ch1, &left, &right);
        if (!(cnt & (1u << 13))) pan_out(3, ch3, &left, &right);
        for (int i = 4; i < 16; i++) pan_out(i, chan_run(i, cyc), &left, &right);
        for (int i = 0; i < 2; i++) if (g_cap[i].cnt & 0x80) {
            s32 v = (i ? right : left) >> 8;
            if (v < -0x8000) v = -0x8000; else if (v > 0x7FFF) v = 0x7FFF;
            cap_run(i, cyc, v);
        }
        int p1 = (int)((g_ch[1].cnt >> 16) & 0x7F), p3 = (int)((g_ch[3].cnt >> 16) & 0x7F);
        if (p1 == 127) p1 = 128; if (p3 == 127) p3 = 128;
        switch (cnt & 0x300) {
        case 0x000: lo = left; break;
        case 0x100: lo = (s32)(((int64_t)ch1 * (128 - p1)) >> 10); break;
        case 0x200: lo = (s32)(((int64_t)ch3 * (128 - p3)) >> 10); break;
        default: lo = (s32)(((int64_t)ch1 * (128 - p1)) >> 10) + (s32)(((int64_t)ch3 * (128 - p3)) >> 10); break;
        }
        switch (cnt & 0xC00) {
        case 0x000: ro = right; break;
        case 0x400: ro = (s32)(((int64_t)ch1 * p1) >> 10); break;
        case 0x800: ro = (s32)(((int64_t)ch3 * p3) >> 10); break;
        default: ro = (s32)(((int64_t)ch1 * p1) >> 10) + (s32)(((int64_t)ch3 * p3) >> 10); break;
        }
    }
    s32 mv = (s32)(cnt & 0x7F); if (mv == 127) mv = 128;
    lo = (s32)(((int64_t)lo * mv) >> 7) >> 8;
    ro = (s32)(((int64_t)ro * mv) >> 7) >> 8;
    if (lo < -0x8000) lo = -0x8000; else if (lo > 0x7FFF) lo = 0x7FFF;
    if (ro < -0x8000) ro = -0x8000; else if (ro > 0x7FFF) ro = 0x7FFF;
    static int bits16 = -1; if (bits16 < 0) bits16 = getenv("DQIX_AUDIO16") != NULL;
    if (!bits16) { lo &= ~0x3F; ro &= ~0x3F; }                     /* the DS's DAC is 10-bit */
    { extern void music_mix(int32_t*, int32_t*); music_mix(&lo, &ro); }   /* a music pack's recordings (music.c), at full depth */
    uint32_t w = g_out_w;
    if (w - g_out_r >= OUT_N) return;                               /* ring full: nobody listening */
    g_out[(w % OUT_N) * 2] = (int16_t)lo;
    g_out[(w % OUT_N) * 2 + 1] = (int16_t)ro;
    g_out_w = w + 1;
}

/* Advance the sound hardware by `cycles` (33.51 MHz bus cycles). */
void sound_advance(uint32_t cycles) {
    while (cycles) {
        uint32_t step = 1024 - g_mix_cycles;
        if (step > cycles) step = cycles;
        g_mix_cycles += step;
        cycles -= step;
        if (g_mix_cycles >= 1024) { g_mix_cycles -= 1024; mix_one(); }
    }
}

/* A write to a channel's control register (offset within 0x400-0x4FF). */
void sound_cnt_written(int n, uint32_t old) {
    (void)old;
    Chan* c = &g_ch[n];
    u32 v = IO7_32_(0x400 + n * 16);
    u32 oldcnt = c->cnt;
    c->cnt = v & 0xFF7F837F;
    if ((v & (1u << 31)) && !(oldcnt & (1u << 31))) c->keyon = 1;
}

int sound_chan_busy(int n) { return (g_ch[n].cnt >> 31) & 1; }

/* Frontend side: take up to n stereo frames; returns how many were available. */
uint32_t sound_read(int16_t* dst, uint32_t n) {
    uint32_t avail = g_out_w - g_out_r;
    if (n > avail) n = avail;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = (g_out_r + i) % OUT_N;
        dst[i * 2] = g_out[k * 2]; dst[i * 2 + 1] = g_out[k * 2 + 1];
    }
    g_out_r += n;
    return n;
}
uint32_t sound_buffered(void) { return g_out_w - g_out_r; }
void sound_discard(void) { g_out_r = g_out_w; }

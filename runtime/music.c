/* Music packs: the game's songs replaced by recordings (OGG or FLAC files in
 * the music folder, named after the song: BG_012.ogg, ME_003.flac).
 *
 * The game plays its music as sequences (notes for the DS's sound chip).
 * When it starts one, the ARM9 sends the sound driver a list of commands;
 * here they are read on the way (music_pxi). A song with a file of its own:
 *   - keeps playing as a sequence, but with every note's velocity at 0, so it
 *     is silent while its timing, loops and the game's waiting on it stay
 *     exactly the same;
 *   - and the file plays instead, from the start, looping between its
 *     LOOPSTART and LOOPSTART + LOOPLENGTH (or LOOPEND) tags in samples,
 *     the usual tags of game music rips. Without tags: songs (BG_) loop the
 *     whole file, jingles (ME_) play once.
 * The game's fades (the player's volume) and stops are followed, and pausing.
 *
 * Songs are recognised by their data, not their address: the sound archive
 * in the ROM (SDAT) is read at the start and every sequence's data hashed.
 * Replays of bug reports leave all of this off, so they stay exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dirent.h>
#include "nds.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"                       /* public domain (Sean Barrett), v1.22 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO_WIDE
#include "dr_flac.h"                          /* public domain / MIT-0 (David Reid), v0.13.3 */

#define OUT_RATE 32728.0                      /* the DS's output rate (33.51 MHz / 1024) */

/* ---------------------------------------------------------------- the songs */
typedef struct {
    char name[32];
    uint32_t rom_off, len;                    /* the sequence's data (after its header) in the ROM */
    uint64_t key, full, mkey, mfull;          /* hashes: first 32 bytes and all, as in the ROM and silenced */
    char file[400];                           /* the pack's file for it ("" none) */
} Song;
static Song* g_songs;
static int g_nsongs;

static uint64_t fnv(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* Every note in a sequence at velocity 0 (in place). Walks the commands in
 * order; a sequence is nothing but commands, so no data is mistaken for one. */
static uint32_t varlen(const uint8_t* d, uint32_t n, uint32_t i) {
    for (int k = 0; k < 4 && i < n; k++) if (!(d[i++] & 0x80)) break;
    return i;
}
static void silence(uint8_t* d, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = d[i++];
        int pre = 0;                                              /* 0xA0 random: last argument is s16 min, max; 0xA1 variable: u8 */
        if (c == 0xA0 || c == 0xA1) { pre = c; if (i >= n) break; c = d[i++]; }
        else if (c == 0xA2) continue;                             /* if: the next command runs or not */
        #define LAST(norm) do { if (pre == 0xA0) i += 4; else if (pre == 0xA1) i += 1; else { norm; } } while (0)
        if (c < 0x80) {                                           /* note: velocity, duration */
            if (i < n) d[i] = 0;
            i++;
            LAST(i = varlen(d, n, i));
        }
        else if (c == 0x80 || c == 0x81) LAST(i = varlen(d, n, i));   /* rest, program */
        else if (c == 0x93) i += 4;                               /* open track */
        else if (c == 0x94 || c == 0x95) i += 3;                  /* jump, call */
        else if (c >= 0xB0 && c <= 0xBD) { i += 1; LAST(i += 2); }   /* variable ops */
        else if (c >= 0xC0 && c <= 0xD7) LAST(i += 1);
        else if (c == 0xE0 || c == 0xE1 || c == 0xE3) LAST(i += 2);
        else if (c == 0xFE) i += 2;                               /* allocate tracks */
        else if (c == 0xFC || c == 0xFD || c == 0xFF) continue;   /* loop end, return, end */
        else break;                                               /* unknown: stop rather than guess */
        #undef LAST
    }
}

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* The sound archives in the ROM: their sequences with their names. */
static void scan_rom(void) {
    if (g_songs || !g_rom || g_rom_size < 0x1000) return;
    const uint8_t magic[8] = { 'S', 'D', 'A', 'T', 0xFF, 0xFE, 0x00, 0x01 };
    for (size_t at = 0; at + 0x40 < g_rom_size; at += 0x200) {   /* files in the ROM start at 512-byte steps */
        if (memcmp(g_rom + at, magic, 8)) continue;
        const uint8_t* s = g_rom + at; uint32_t ssize = rd32(s + 8);
        if (ssize < 0x40 || at + ssize > g_rom_size) continue;
        uint32_t symb = rd32(s + 0x10), info = rd32(s + 0x18), fat = rd32(s + 0x20);
        if (!info || !fat || info + 0x40 > ssize || fat + 12 > ssize) continue;
        uint32_t irec = info + rd32(s + info + 8);                /* INFO: record 0, the sequences */
        if (irec + 4 > ssize) continue;
        uint32_t nseq = rd32(s + irec), nfat = rd32(s + fat + 8);
        if (nseq > 4096 || irec + 4 + nseq * 4 > ssize || fat + 12 + nfat * 16 > ssize) continue;
        uint32_t srec = 0, nsym = 0;
        if (symb && symb + 0x40 < ssize) { srec = symb + rd32(s + symb + 8); if (srec + 4 < ssize) nsym = rd32(s + srec); if (srec + 4 + nsym * 4 > ssize) nsym = 0; }
        for (uint32_t k = 0; k < nseq; k++) {
            uint32_t p = rd32(s + irec + 4 + k * 4);
            if (!p || info + p + 8 > ssize) continue;
            uint32_t fid = rd32(s + info + p) & 0xFFFFFF;
            if (fid >= nfat) continue;
            uint32_t off = rd32(s + fat + 12 + fid * 16), len = rd32(s + fat + 12 + fid * 16 + 4);
            if (off + len > ssize || len < 0x20 || memcmp(s + off, "SSEQ", 4)) continue;
            uint32_t doff = rd32(s + off + 0x18);
            if (doff >= len) continue;
            Song g; memset(&g, 0, sizeof g);
            const char* nm = NULL;
            if (k < nsym) { uint32_t np = rd32(s + srec + 4 + k * 4); if (np && symb + np < ssize) nm = (const char*)s + symb + np; }
            if (nm) { size_t l = 0; while (l < sizeof g.name - 1 && nm[l] && symb + (uint32_t)(nm - (const char*)s - symb) + l < ssize) { char ch = nm[l]; g.name[l] = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ? ch : '_'; l++; } }
            if (!g.name[0]) snprintf(g.name, sizeof g.name, "SEQ_%03u", k);
            g.rom_off = (uint32_t)at + off + doff; g.len = len - doff;
            const uint8_t* d = g_rom + g.rom_off;
            g.key = fnv(d, g.len < 32 ? g.len : 32); g.full = fnv(d, g.len);
            uint8_t* m = malloc(g.len);
            if (!m) continue;
            memcpy(m, d, g.len); silence(m, g.len);
            g.mkey = fnv(m, g.len < 32 ? g.len : 32); g.mfull = fnv(m, g.len);
            free(m);
            Song* ng = realloc(g_songs, sizeof(Song) * (size_t)(g_nsongs + 1));
            if (!ng) break;
            g_songs = ng; g_songs[g_nsongs++] = g;
        }
    }
}

/* main RAM, as the ARM9 sees it */
static uint8_t* ram(uint32_t a, uint32_t n) {
    if ((a >> 24) != 0x02) return NULL;
    a &= MAIN_RAM_SIZE - 1;
    if (a + n > MAIN_RAM_SIZE) return NULL;
    return g_main_ram + a;
}

/* which song is at this address (silenced already or not): -1 none */
static int find_song(uint32_t a, int* muted) {
    scan_rom();
    uint8_t* p = ram(a, 32);
    if (!p) return -1;
    uint64_t k = fnv(p, 32);
    for (int i = 0; i < g_nsongs; i++) {
        Song* g = &g_songs[i];
        if (g->len < 32) continue;
        if (k != g->key && k != g->mkey) continue;
        uint8_t* d = ram(a, g->len);
        if (!d) continue;
        uint64_t f = fnv(d, g->len);
        if (f == g->full) { *muted = 0; return i; }
        if (f == g->mfull) { *muted = 1; return i; }
    }
    return -1;
}

/* the copies silenced in RAM, to give back their notes when a song loses its file */
typedef struct { uint32_t addr; int song; } Muted;
static Muted g_muted[64];
static int g_nmuted;

static void unmute_where_no_file(int all) {
    int k = 0;
    for (int i = 0; i < g_nmuted; i++) {
        Muted m = g_muted[i]; Song* g = &g_songs[m.song];
        if (!all && g->file[0]) { g_muted[k++] = m; continue; }
        uint8_t* d = ram(m.addr, g->len);
        if (d && fnv(d, g->len) == g->mfull) memcpy(d, g_rom + g->rom_off, g->len);   /* still there, still ours */
    }
    g_nmuted = k;
}

/* ---------------------------------------------------------------- streams */
typedef struct {
    int on, player, song, paused, loop, ended;
    stb_vorbis* v; drflac* f;
    FILE* wf; uint32_t wdata, wframes; int wbits, wfloat;   /* WAV: where its samples start, how many, their format */
    int ch; double rate;
    uint64_t pos;                             /* frames read from the file */
    uint64_t loop_start, loop_end;            /* loop_end 0: the file's end */
    float buf[2048 * 8]; int bn, bi;          /* decoded frames (interleaved, ch channels) */
    float prev[2], cur[2]; double t;          /* resampling */
    float gain;
    int base; uint32_t out;                   /* the game's volume when it started (the recording keeps its own level), samples played */
} Stream;
#define NSTREAM 4
static Stream g_st[NSTREAM];
static int16_t g_fader[16];                   /* each player's volume from the game (extFader, 1/10 dB) */
static int g_on, g_volume = 70;
static char g_dir[300] = "music";
static char g_now[64];
static int g_announce;

static void stream_close(Stream* s) {
    if (s->v) stb_vorbis_close(s->v);
    if (s->f) drflac_close(s->f);
    if (s->wf) fclose(s->wf);
    memset(s, 0, sizeof *s);
}

static int prefix(const char* c, const char* key) {          /* "KEY=" at the start, any case */
    for (; *key; c++, key++) { char a = *c, b = *key; if (a >= 'a' && a <= 'z') a -= 32; if (a != b) return 0; }
    return 1;
}
static void tags_from(Stream* s, const char* c) {
    if (prefix(c, "LOOPSTART=")) { s->loop_start = strtoull(c + 10, NULL, 10); s->loop = 1; }
    else if (prefix(c, "LOOPLENGTH=")) { s->loop_end = s->loop_start + strtoull(c + 11, NULL, 10); s->loop = 1; }   /* (after LOOPSTART) */
    else if (prefix(c, "LOOPEND=")) { s->loop_end = strtoull(c + 8, NULL, 10); s->loop = 1; }
}
static void flac_meta(void* u, drflac_metadata* m) {
    Stream* s = u;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it; drflac_uint32 len; const char* c;
    char tmp[128], start[128] = "", rest[2][128] = { "", "" };
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
    while ((c = drflac_next_vorbis_comment(&it, &len))) {
        if (len >= sizeof tmp) continue;
        memcpy(tmp, c, len); tmp[len] = 0;
        if (prefix(tmp, "LOOPSTART=")) strcpy(start, tmp);
        else if (prefix(tmp, "LOOPLENGTH=")) strcpy(rest[0], tmp);
        else if (prefix(tmp, "LOOPEND=")) strcpy(rest[1], tmp);
    }
    if (start[0]) tags_from(s, start);
    for (int i = 0; i < 2; i++) if (rest[i][0]) tags_from(s, rest[i]);
}

/* WAV (PCM 8/16/24/32-bit or float): the music export's files, with their loop
 * in the "smpl" chunk (as samplers and editors write it: the end is the last sample in it) */
static int wav_open(Stream* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[12]; if (fread(h, 1, 12, f) != 12 || memcmp(h + 8, "WAVE", 4)) { fclose(f); return 0; }
    int fmt = 0;
    for (;;) {
        uint8_t c[8]; if (fread(c, 1, 8, f) != 8) break;
        uint32_t n = rd32(c + 4); long at = ftell(f);
        if (!memcmp(c, "fmt ", 4) && n >= 16) {
            uint8_t b[40] = { 0 }; if (fread(b, 1, n < 40 ? n : 40, f) < 16) break;
            int tag = b[0] | b[1] << 8; s->ch = b[2] | b[3] << 8; s->rate = rd32(b + 4); s->wbits = b[14] | b[15] << 8;
            if (tag == 0xFFFE && n >= 26) tag = b[24] | b[25] << 8;          /* extensible: the sub-format */
            s->wfloat = tag == 3; fmt = (tag == 1 || tag == 3);
        } else if (!memcmp(c, "data", 4)) { s->wdata = (uint32_t)at; s->wframes = n; }
        else if (!memcmp(c, "smpl", 4) && n >= 60) {
            uint8_t b[60]; if (fread(b, 1, 60, f) == 60 && rd32(b + 28) >= 1) { s->loop = 1; s->loop_start = rd32(b + 44); s->loop_end = (uint64_t)rd32(b + 48) + 1; }
        }
        if (fseek(f, at + (long)n + (long)(n & 1), SEEK_SET)) break;
    }
    int bps = s->wbits / 8;
    if (!fmt || !s->wdata || s->ch < 1 || s->ch > 8 || !(bps == 1 || bps == 2 || bps == 3 || bps == 4) || (s->wfloat && bps != 4)) { fclose(f); return 0; }
    s->wframes /= (uint32_t)(bps * s->ch);
    s->wf = f; fseek(f, (long)s->wdata, SEEK_SET);
    return 1;
}
static int wav_read(Stream* s, float* out, int frames) {
    int bps = s->wbits / 8, n = 0;
    if (s->pos >= s->wframes) return 0;
    if ((uint64_t)frames > s->wframes - s->pos) frames = (int)(s->wframes - s->pos);
    uint8_t tmp[4096 * 4];
    while (n < frames) {
        int k = frames - n, per = bps * s->ch; if (k * per > (int)sizeof tmp) k = (int)sizeof tmp / per;
        int got = (int)fread(tmp, (size_t)per, (size_t)k, s->wf);
        if (got <= 0) break;
        for (int i = 0; i < got * s->ch; i++) {
            const uint8_t* p = tmp + i * bps; float v;
            if (bps == 1) v = ((int)p[0] - 128) / 128.0f;
            else if (bps == 2) v = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
            else if (bps == 3) v = (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) / 8388608.0f;
            else if (s->wfloat) { memcpy(&v, p, 4); }
            else v = (float)(int32_t)rd32(p) / 2147483648.0f;
            out[n * s->ch + i] = v;
        }
        n += got;
    }
    return n;
}

static int stream_open(Stream* s, const char* path, int song) {
    memset(s, 0, sizeof *s);
    s->song = song;
    char magic[4] = { 0 };                                       /* by what the file is, not its name */
    { FILE* f = fopen(path, "rb"); if (!f) return 0; if (fread(magic, 1, 4, f) != 4) magic[0] = 0; fclose(f); }
    if (!memcmp(magic, "RIFF", 4)) { if (!wav_open(s, path)) return 0; }
    else if (!memcmp(magic, "fLaC", 4) || (!memcmp(magic, "ID3", 3) && strstr(path, ".flac"))) {
        s->f = drflac_open_file_with_metadata(path, flac_meta, s, NULL);
        if (!s->f) return 0;
        s->ch = s->f->channels; s->rate = s->f->sampleRate;
        if (s->ch > 8) { drflac_close(s->f); s->f = NULL; return 0; }
    } else {
        int err = 0;
        s->v = stb_vorbis_open_filename(path, &err, NULL);
        if (!s->v) return 0;
        stb_vorbis_info vi = stb_vorbis_get_info(s->v);
        s->ch = vi.channels; s->rate = vi.sample_rate;
        if (s->ch > 8) { stb_vorbis_close(s->v); s->v = NULL; return 0; }
        stb_vorbis_comment cm = stb_vorbis_get_comment(s->v);
        char start[128] = "", rest[2][128] = { "", "" };
        for (int i = 0; i < cm.comment_list_length; i++) {
            const char* c = cm.comment_list[i];
            if (strlen(c) >= 128) continue;
            if (prefix(c, "LOOPSTART=")) strcpy(start, c);
            else if (prefix(c, "LOOPLENGTH=")) strcpy(rest[0], c);
            else if (prefix(c, "LOOPEND=")) strcpy(rest[1], c);
        }
        if (start[0]) tags_from(s, start);
        for (int i = 0; i < 2; i++) if (rest[i][0]) tags_from(s, rest[i]);
    }
    if (!s->loop) s->loop = !strncmp(g_songs[song].name, "BG", 2);  /* no tags: songs loop, jingles don't */
    if (s->loop_end && s->loop_end <= s->loop_start) s->loop_end = 0;
    s->on = 1; s->gain = 0.0f; s->t = 1.0;
    return 1;
}

static int seek_to(Stream* s, uint64_t frame) {
    s->bn = s->bi = 0; s->pos = frame;
    if (s->v) return stb_vorbis_seek(s->v, (unsigned)frame);
    if (s->f) return drflac_seek_to_pcm_frame(s->f, frame);
    if (s->wf) { if (frame > s->wframes) return 0; return fseek(s->wf, (long)(s->wdata + frame * (uint64_t)(s->wbits / 8 * s->ch)), SEEK_SET) == 0; }
    return 0;
}

/* the next frame from the file (looping); 0 at its end */
static int next_frame(Stream* s, float* l, float* r) {
    for (int tries = 0; tries < 3; tries++) {
        if (s->bi < s->bn) {
            const float* f = s->buf + (size_t)s->bi * s->ch;
            *l = f[0]; *r = s->ch > 1 ? f[1] : f[0];
            s->bi++;
            return 1;
        }
        int want = 2048;
        if (s->loop && s->loop_end) {
            if (s->pos >= s->loop_end) { seek_to(s, s->loop_start); continue; }
            if (s->loop_end - s->pos < (uint64_t)want) want = (int)(s->loop_end - s->pos);
        }
        int got = 0;
        if (s->v) got = stb_vorbis_get_samples_float_interleaved(s->v, s->ch, s->buf, want * s->ch);
        else if (s->f) got = (int)drflac_read_pcm_frames_f32(s->f, (drflac_uint64)want, s->buf);
        else if (s->wf) got = wav_read(s, s->buf, want);
        if (got <= 0) {
            if (!s->loop) return 0;
            if (!seek_to(s, s->loop_start) && s->loop_start) seek_to(s, 0);
            continue;
        }
        s->bn = got; s->bi = 0; s->pos += (uint64_t)got;
    }
    return 0;
}

/* ---------------------------------------------------------------- the game's commands */
int music_reload(void);
void music_set_folder(const char* dir);
static void start(int player, int song) {
    for (int i = 0; i < NSTREAM; i++) if (g_st[i].on && g_st[i].player == player) stream_close(&g_st[i]);
    if (song < 0 || !g_songs[song].file[0]) return;
    Stream* s = NULL;
    for (int i = 0; i < NSTREAM && !s; i++) if (!g_st[i].on) s = &g_st[i];
    if (!s) { s = &g_st[0]; stream_close(s); }
    if (!stream_open(s, g_songs[song].file, song)) { fprintf(stderr, "music: can't play %s\n", g_songs[song].file); stream_close(s); return; }
    s->player = player; s->base = g_fader[player & 15];
}

static int g_prepared[16];
static int lg_on(void) { static int l = -1; if (l < 0) l = getenv("DQIX_MUSICLOG") != NULL; return l; }
static void seq_cmd(int player, uint32_t base, int prepare) {
    player &= 15;
    int muted = 0, song = find_song(base, &muted);
    if (lg_on()) fprintf(stderr, "music f%llu player %d %s %s%s\n", (unsigned long long)g_frame, player, prepare ? "prepare" : "start",
                         song >= 0 ? g_songs[song].name : "(not a song)", song >= 0 && g_songs[song].file[0] && g_on ? " (from the pack)" : "");
    if (song >= 0) {
        snprintf(g_now, sizeof g_now, "%s", g_songs[song].name); g_announce = 1;
        if (g_on && g_songs[song].file[0] && !muted) {
            uint8_t* d = ram(base, g_songs[song].len);
            if (d) {
                silence(d, g_songs[song].len);
                int dup = 0;
                for (int i = 0; i < g_nmuted; i++) if (g_muted[i].addr == base) dup = 1;
                if (!dup) { if (g_nmuted == 64) { memmove(g_muted, g_muted + 1, sizeof(Muted) * 63); g_nmuted--; } g_muted[g_nmuted++] = (Muted){ base, song }; }
            }
        }
        else if (muted && (!g_on || !g_songs[song].file[0])) {    /* silenced earlier, no file now: the notes back */
            uint8_t* d = ram(base, g_songs[song].len);
            if (d) memcpy(d, g_rom + g_songs[song].rom_off, g_songs[song].len);
        }
    }
    if (prepare) g_prepared[player] = song + 1;
    else start(player, g_on ? song : -1);
}

/* An ARM9 -> ARM7 word on the sound channel: the address of a command list
 * (SNDCommand: next, id, arg[4]). */
void music_pxi(uint32_t data) {
    static int lg = -1;
    if (lg < 0) {                                                       /* tests: DQIX_MUSIC=folder (also in replays), DQIX_MUSICLOG */
        lg = getenv("DQIX_MUSICLOG") != NULL;
        if (getenv("DQIX_MUSIC")) { music_set_folder(getenv("DQIX_MUSIC")); g_on = 1; fprintf(stderr, "music: %d songs in the pack\n", music_reload()); }
    }
    if (!g_on && !g_nmuted && !lg) return;
    uint32_t a = data;
    if ((a >> 24) != 0x02) return;
    for (int n = 0; a && n < 256; n++) {
        uint8_t* c = ram(a, 24);
        if (!c) break;
        uint32_t next = rd32(c), id = rd32(c + 4), a0 = rd32(c + 8), a1 = rd32(c + 12), a2 = rd32(c + 16), a3 = rd32(c + 20);
        if (lg && id != 6 && id < 16) fprintf(stderr, "snd f%llu cmd %u %08x %08x %08x %08x\n", (unsigned long long)g_frame, id, a0, a1, a2, a3);
        if (lg && id == 6) fprintf(stderr, "snd f%llu param players %x offset %u data %d size %u\n", (unsigned long long)g_frame, a0, a1, (int)a2, a3);
        switch (id) {
        case 0: seq_cmd((int)a0, a1, 0); break;                    /* START_SEQ: player, data, offset, bank */
        case 1: for (int i = 0; i < NSTREAM; i++) if (g_st[i].on && g_st[i].player == (int)(a0 & 15)) stream_close(&g_st[i]); break;   /* STOP_SEQ */
        case 2: seq_cmd((int)a0, a1, 1); break;                    /* PREPARE_SEQ */
        case 3: { int p = (int)(a0 & 15); if (g_prepared[p]) start(p, g_on ? g_prepared[p] - 1 : -1); g_prepared[p] = 0; break; }   /* START_PREPARED_SEQ */
        case 4: for (int i = 0; i < NSTREAM; i++) if (g_st[i].on && g_st[i].player == (int)(a0 & 15)) g_st[i].paused = a1 != 0; break;   /* PAUSE_SEQ */
        case 6:                                                         /* PLAYER_PARAM: player, offset, data, size */
            if (a1 == 6 && a3 == 2 && a0 < 16) { int p = (int)a0;      /* extFader (this SDK: the player's number, not a mask) */
                g_fader[p] = (int16_t)a2;
                for (int i = 0; i < NSTREAM; i++) if (g_st[i].on && g_st[i].player == p && g_st[i].out < 4096) g_st[i].base = g_fader[p];   /* (set as it starts) */
            }
            break;
        }
        a = next;
    }
}

/* One output sample (called at the DS's rate): the streams added in. */
void music_mix(int32_t* l, int32_t* r) {
    float sl = 0, sr = 0; int any = 0;
    for (int i = 0; i < NSTREAM; i++) {
        Stream* s = &g_st[i];
        if (!s->on || s->paused) continue;
        any = 1;
        double step = s->rate / OUT_RATE;
        s->t += step;
        while (s->t >= 1.0) {
            s->prev[0] = s->cur[0]; s->prev[1] = s->cur[1];
            if (!next_frame(s, &s->cur[0], &s->cur[1])) { s->cur[0] = s->cur[1] = 0; if (++s->ended > 1) { s->on = 0; break; } }
            s->t -= 1.0;
        }
        if (!s->on) { stream_close(s); continue; }
        /* the game's fades: relative to its volume at the start (a recording keeps its own mix) */
        int fd = g_fader[s->player & 15], base = s->base < -300 ? -70 : s->base;   /* (started silent: fading in to the usual level) */
        float target = fd <= -720 ? 0.0f : fminf(1.0f, powf(10.0f, (float)(fd - base) / 200.0f));
        s->out++;
        s->gain += (target - s->gain) * 0.002f;                         /* smooth: no clicks when the game steps the volume */
        float t = (float)s->t;
        sl += (s->prev[0] + (s->cur[0] - s->prev[0]) * t) * s->gain;
        sr += (s->prev[1] + (s->cur[1] - s->prev[1]) * t) * s->gain;
    }
    if (!any) return;
    float v = (float)g_volume / 100.0f * 32767.0f;
    int32_t a = *l + (int32_t)lrintf(sl * v), b = *r + (int32_t)lrintf(sr * v);
    *l = a < -32768 ? -32768 : a > 32767 ? 32767 : a;
    *r = b < -32768 ? -32768 : b > 32767 ? 32767 : b;
}

/* ---------------------------------------------------------------- the pack */

/* the pack's file for a song: named after it (BG_012.ogg), and also BG_012.ogg.flac
 * (renamed with Windows hiding the extension) or "BG_012 - Title.flac". */
static int name_match(const char* file, const char* song) {
    size_t n = strlen(song), l = strlen(file);
    for (size_t i = 0; i < n; i++) { char a = file[i], b = song[i]; if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; if (!a || a != b) return 0; }
    char c = file[n];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return 0;   /* BG_01 is not BG_012 */
    return (l > 4 && (prefix(file + l - 4, ".OGG") || prefix(file + l - 4, ".WAV"))) || (l > 5 && prefix(file + l - 5, ".FLAC"));
}

int music_reload(void) {
    scan_rom();
    for (int i = 0; i < g_nsongs; i++) g_songs[i].file[0] = 0;
    DIR* d = opendir(g_dir);
    struct dirent* e;
    while (d && (e = readdir(d))) {
        for (int i = 0; i < g_nsongs; i++) {
            Song* g = &g_songs[i];
            if (!name_match(e->d_name, g->name)) continue;
            char p[400]; snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
            int exact = strlen(e->d_name) <= strlen(g->name) + 5;      /* BG_012.ogg / .flac first */
            if (!g->file[0] || exact) snprintf(g->file, sizeof g->file, "%s", p);
        }
    }
    if (d) closedir(d);
    int n = 0;
    for (int i = 0; i < g_nsongs; i++) n += g_songs[i].file[0] != 0;
    unmute_where_no_file(!g_on);
    for (int i = 0; i < NSTREAM; i++) if (g_st[i].on && (!g_on || !g_songs[g_st[i].song].file[0])) stream_close(&g_st[i]);
    return n;
}

void music_set_folder(const char* dir) { if (dir && *dir) snprintf(g_dir, sizeof g_dir, "%s", dir); }
void music_set_volume(int v) { g_volume = v < 0 ? 0 : v > 200 ? 200 : v; }
int music_enable(int on) {
    if (getenv("DQIX_INPUT_FILE") && !getenv("DQIX_MUSIC_IN_REPLAYS")) on = 0;   /* replays stay exact */
    g_on = on != 0;
    return music_reload();
}
int music_song_count(void) { scan_rom(); return g_nsongs; }

/* The song that started last (for "show the song's name"); 1 once per new song. */
int music_now(char* out, int n) {
    snprintf(out, (size_t)n, "%s", g_now);
    int a = g_announce; g_announce = 0;
    return a;
}

/* songs.txt: every song's name, and which have a file in the pack */
int music_write_list(const char* path) {
    scan_rom();
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# The game's songs. Put a recording in this folder named after one to play it instead:\n"
               "# BG_012.ogg or BG_012.flac (a title after the name is fine: BG_012 - Town.flac). Loop points: the tags LOOPSTART and LOOPLENGTH (or LOOPEND), in samples.\n"
               "# Without them, songs (BG_) loop the whole file and jingles (ME_) play once.\n"
               "# Mods > Show each song's name tells you which one is playing; Mods > Export the game's songs writes them all as WAV files to export.\n\n");
    for (int i = 0; i < g_nsongs; i++)
        fprintf(f, "%-10s %s%s\n", g_songs[i].name, !strncmp(g_songs[i].name, "ME", 2) ? "jingle" : "song  ", g_songs[i].file[0] ? "   (in the pack)" : "");
    fclose(f);
    return 1;
}

/* Headless frontend: no window. Audio can be written to a .wav file with
 * DQIX_WAV=path (for testing); otherwise it is discarded. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "nds.h"
#include "frontend.h"
#include "gpu3d.h"

/* DQIX_NO3D: build 3D frames but do not draw them (measures what a GPU renderer leaves on the CPU). */
static void null_vblank(void) {}
static void null_render(const Frame3D* f) { (void)f; }
static const Renderer3D g_null3d = { null_vblank, null_render };

static FILE* g_wav;
static uint32_t g_wav_frames;

static void wav_header(FILE* f, uint32_t frames) {
    uint32_t rate = 32768, bytes = frames * 4;
    uint32_t h[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, bytes };
    fseek(f, 0, SEEK_SET);
    fwrite(h, 4, 11, f);
    fseek(f, 0, SEEK_END);
}

static void wav_close(void) {
    if (!g_wav) return;
    wav_header(g_wav, g_wav_frames);
    fclose(g_wav);
    g_wav = NULL;
}

void frontend_init(int argc, char** argv) {
    (void)argc; (void)argv;
    const char* p = getenv("DQIX_WAV");
    if (p && (g_wav = fopen(p, "wb"))) { wav_header(g_wav, 0); atexit(wav_close); }
    if (getenv("DQIX_NO3D")) g_renderer3d = &g_null3d;
}

/* DQIX_FRAMETIME=from_frame: histogram of host time per frame (performance checks) */
#ifdef _WIN32
#include <windows.h>
static double now_ms(void) { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
#else
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#endif
static int g_ft = -1; static unsigned long long g_ft_from; static double g_ft_last; static unsigned g_hist[8]; static double g_ft_max; static unsigned long long g_ft_maxf;
static unsigned g_slow_n; static double g_slow_3d, g_slow_2d, g_slow_tot;
static void ft_report(void) {
    if (g_slow_n) fprintf(stderr, "slow frames (>=25 ms): %u, average %.1f ms: 3D %.1f, 2D %.1f, rest (game code) %.1f\n", g_slow_n, g_slow_tot / g_slow_n,
                          g_slow_3d / g_slow_n, g_slow_2d / g_slow_n, (g_slow_tot - g_slow_3d - g_slow_2d) / g_slow_n);
    static const char* lab[8] = { "<4ms", "4-8", "8-12", "12-16.7", "16.7-25", "25-33", "33-50", ">50" };
    fprintf(stderr, "frame times:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %s:%u", lab[i], g_hist[i]);
    extern double g_t3d_ms, g_t2d_ms;
    fprintf(stderr, "  max %.1f ms at frame %llu\n", g_ft_max, g_ft_maxf);
    extern double g_t3d_part[4];
    fprintf(stderr, "total: 3D render %.0f ms (clear %.0f, polygons %.0f, edges %.0f, fog %.0f), 2D render %.0f ms\n", g_t3d_ms,
            g_t3d_part[0], g_t3d_part[1], g_t3d_part[2], g_t3d_part[3], g_t2d_ms);
}
static void frame_time(void) {
    if (g_ft < 0) { const char* e = getenv("DQIX_FRAMETIME"); g_ft = e != NULL; if (e) { g_ft_from = strtoull(e, NULL, 10); atexit(ft_report); } }
    if (!g_ft) return;
    double t = now_ms();
    if (g_frame > g_ft_from && g_ft_last > 0) {
        double d = t - g_ft_last;
        extern double g_t3d_ms, g_t2d_ms;
        static double p3, p2;
        if (d >= 25) { g_slow_n++; g_slow_3d += g_t3d_ms - p3; g_slow_2d += g_t2d_ms - p2; g_slow_tot += d; }
        p3 = g_t3d_ms; p2 = g_t2d_ms;
        int b = d < 4 ? 0 : d < 8 ? 1 : d < 12 ? 2 : d < 16.7 ? 3 : d < 25 ? 4 : d < 33 ? 5 : d < 50 ? 6 : 7;
        g_hist[b]++;
        if (d > g_ft_max) { g_ft_max = d; g_ft_maxf = g_frame; }
        static double slow_ms = -1; if (slow_ms < 0) { const char* e = getenv("DQIX_SLOWLOG"); slow_ms = e ? atof(e) : 0; }
        { extern double g_t3d_ms, g_t2d_ms; static double q3, q2;
          if (slow_ms > 0 && d >= slow_ms) fprintf(stderr, "slow frame %llu: %.1f ms (3D %.1f, 2D %.1f)\n", (unsigned long long)g_frame, d, g_t3d_ms - q3, g_t2d_ms - q2);
          q3 = g_t3d_ms; q2 = g_t2d_ms; }
    }
    g_ft_last = t;
}

/* DQIX_PACE=frame: from that frame on, wait for real time like the window does (tests) */
static void pace(void) {
    static long long from = -2; static double next;
    if (from == -2) { const char* e = getenv("DQIX_PACE"); from = e ? atoll(e) : -1; }
    if (from < 0 || (long long)g_frame < from) return;
    double t = now_ms();
    if (next == 0 || t > next + 100) next = t;
    next += 1000.0 / 59.8261;
    while ((t = now_ms()) < next) {
        double left = next - t;
#ifdef _WIN32
        if (left > 2.5) Sleep((DWORD)(left - 2));
#else
        if (left > 2.5) { struct timespec ts = { 0, (long)((left - 2) * 1e6) }; nanosleep(&ts, NULL); }
#endif
    }
}

void frontend_frame(void) {
    frame_time();
    pace();
    int16_t buf[2048 * 2];
    uint32_t n;
    while ((n = sound_read(buf, 2048)) > 0)
        if (g_wav) { fwrite(buf, 4, n, g_wav); g_wav_frames += n; }
}

void frontend_shutdown(void) { wav_close(); }

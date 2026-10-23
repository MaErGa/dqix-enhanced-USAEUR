/* DQIX_SAMPLE="first_frame,last_frame[,file]": sampling profiler for test builds
 * (Linux only). Every 50 us inside those frames, records the native
 * instruction address; resolve them with `nm` afterwards. */
#if defined(__linux__) && defined(__x86_64__)
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include "nds.h"

#define SAMPLE_MAX (1 << 22)
static uintptr_t* g_samples;
static volatile unsigned g_nsamples;
static unsigned long long g_from, g_to;
static char g_out[400] = "samples.txt";

static timer_t g_timer;
static int g_fast;
static void set_rate(int fast) {              /* 50 us inside the window, 10 ms outside (cheap) */
    struct itimerspec its = { { 0, fast ? 50000 : 10000000 }, { 0, fast ? 50000 : 10000000 } };
    timer_settime(g_timer, 0, &its, NULL);
    g_fast = fast;
}

static void on_prof(int sig, siginfo_t* si, void* ctx) {
    (void)sig; (void)si;
    int in = g_frame + 1 >= g_from && g_frame <= g_to;
    if (in != g_fast) set_rate(in);
    if (g_frame < g_from || g_frame > g_to || g_nsamples >= SAMPLE_MAX) return;
    g_samples[g_nsamples++] = (uintptr_t)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
}

static void write_samples(void) {
    FILE* f = fopen(g_out, "w");
    if (!f) return;
    extern char __executable_start __attribute__((weak));   /* lets the PIE base be removed (absent in a library) */
    fprintf(f, "base %lx\n", (unsigned long)(uintptr_t)&__executable_start);
    for (unsigned i = 0; i < g_nsamples; i++) fprintf(f, "%lx\n", (unsigned long)g_samples[i]);
    fclose(f);
    fprintf(stderr, "sampler: %u samples -> %s\n", g_nsamples, g_out);
}

__attribute__((constructor)) static void sampler_init(void) {
    const char* e = getenv("DQIX_SAMPLE");
    if (!e || sscanf(e, "%llu,%llu", &g_from, &g_to) != 2) return;
    const char* c = strchr(e, ','); c = c ? strchr(c + 1, ',') : NULL;
    if (c) snprintf(g_out, sizeof g_out, "%s", c + 1);
    g_samples = malloc(sizeof(uintptr_t) * SAMPLE_MAX);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct sigevent sev; memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL; sev.sigev_signo = SIGPROF;
    if (timer_create(CLOCK_MONOTONIC, &sev, &g_timer) == 0) set_rate(0);
    atexit(write_samples);
}
#endif

/* Running as one of several consoles inside another program (sessions: the
 * other players' DSes all run on one thread of the player's game). The
 * whole game + runtime is then a shared library (dqixcore.dll), loaded once
 * per console from its own copy of the file so each has its own memory. The
 * host gives each copy its settings (instead of the environment), a way to
 * wait (yield: let the other consoles run) and to stop (exit: this console
 * only), and the ROM it already has in memory. Standalone, all of this
 * falls back to the usual getenv / exit. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#undef getenv

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static const CoreHost* g_host;

EXPORT void dqix_core_setup(const CoreHost* h) { g_host = h; }
int dqix_main(int argc, char** argv);
EXPORT int dqix_core_main(int argc, char** argv) { return dqix_main(argc, argv); }

int rt_hosted(void) { return g_host != NULL; }

char* rt_getenv(const char* name) {
    if (!g_host) return getenv(name);
    size_t n = strlen(name);
    for (const char* const* e = g_host->env; e && *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return (char*)*e + n + 1;
    if (!strncmp(name, "DQIX_", 5)) return NULL;               /* the host's own settings aren't this console's */
    return getenv(name);
}

void air_leave_now(void);
_Noreturn void rt_exit(int code) {
    if (g_host && g_host->exit_console) {
        backup_flush();
        air_leave_now();
        g_host->exit_console(code);                             /* doesn't come back */
    }
    exit(code);
}

int rt_yield(void) {
    if (!g_host || !g_host->yield) return 0;
    g_host->yield();
    return 1;
}

int rt_host_rom(const uint8_t** rom, size_t* size) {
    if (!g_host || !g_host->rom) return 0;
    *rom = g_host->rom; *size = g_host->rom_size;
    return 1;
}

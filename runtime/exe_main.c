/* The program's entry: the game itself is dqix_main (main.c), so the same
 * code can also run as a library (hosting.c).
 *
 * Started with no arguments (dqix.exe double-clicked, or a shortcut to it), it
 * plays the game in its own folder like play.bat does. On Windows the console
 * window that would open with it is closed, and what the game prints goes to
 * last-run.log instead. */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

int dqix_main(int argc, char** argv);

static void to_own_folder(const char* argv0) {
    char dir[1024];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) return;
    char* s = strrchr(dir, '\\');
#else
    snprintf(dir, sizeof dir, "%s", argv0);
    char* s = strrchr(dir, '/');
#endif
    if (s) { *s = 0; chdir(dir); }
}

int main(int argc, char** argv) {
    if (argc == 1) {
        to_own_folder(argv[0]);
        FILE* f = fopen("data/dqix.nds", "rb");
        if (f) {
            fclose(f);
#ifdef _WIN32
            DWORD procs[2];
            if (GetConsoleProcessList(procs, 2) <= 1) {      /* a console of our own (not cmd's), or none */
                FreeConsole();
                freopen("last-run.log", "w", stdout);
                freopen("last-run.log", "a", stderr);
                setvbuf(stderr, NULL, _IONBF, 0);
            }
#endif
            static char* def[] = {NULL, "data/dqix.nds", "data/code", "0", "1", "dqix.sav", NULL};
            def[0] = argv[0];
            return dqix_main(6, def);
        }
    }
    return dqix_main(argc, argv);
}

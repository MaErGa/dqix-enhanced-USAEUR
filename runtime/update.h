/* Updates from the project's GitHub repository (Windows).
 *
 * The repository holds no game: an update downloads the new version of the
 * project (its source), then the one-click build (BUILD.bat) makes the game
 * again from the player's own ROM. The game folder (saves, settings,
 * reports) is kept; the project files it replaces are copied to
 * update\previous first, with update\rollback.bat to go back.
 *
 * stable = the "stable" branch, nightly = "main". The version of each is read
 * from its runtime/version.h. */
#pragma once
#include <stddef.h>

#define UPDATE_REPO "sackchief/dqix-enhanced"

typedef struct {
    int ok;                       /* both versions were read */
    char stable[32], nightly[32]; /* e.g. "v27", "v27-nightly9" */
    char error[160];
} UpdateInfo;

/* 1 if a > b (v27 > v27-nightly9 > v27-nightly8 > v26) */
int update_newer(const char* a, const char* b);

/* in the background; update_poll() returns 1 once, when it has finished */
void update_check_async(void);
int update_poll(UpdateInfo* out);
/* now (waits for the answer, a second or two) */
void update_check_now(UpdateInfo* out);

/* Where the project is (the folder with BUILD.bat, one up from the game's),
 * or NULL: then updating isn't possible (a developer build, say). */
const char* update_project_dir(void);

/* Download `branch` ("stable" or "main") and hand over to the updater, which
 * waits for this program to close. 1: the caller must now quit. */
int update_install(const char* branch, const char* version, char* error, size_t n);

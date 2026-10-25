/* Saves from other emulators. The game uses the DS's own save: 64 KB, starting with
 * "DRAGON QUEST IX". DeSmuME writes .dsv/.duc files: the same data (without its trailing
 * 0xFF bytes) followed by a footer of 122 bytes ending in "|-DESMUME SAVE-|". */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SAVE_SIZE 65536u

enum {
    SAVE_OK = 0,
    SAVE_BAD_SIZE,         /* not a size a DQIX save can have */
    SAVE_NOT_DQIX,         /* doesn't start with "DRAGON QUEST IX" */
    SAVE_EMPTY,            /* the right kind of file, but no adventure in it */
};

/* Turns the bytes of a save file (raw, or with DeSmuME's footer) into the 64 KB the game uses.
 * `out` must hold SAVE_SIZE bytes. Returns SAVE_OK or the reason. */
int save_convert(const uint8_t* in, size_t n, uint8_t* out);

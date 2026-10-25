/* Saves from other emulators (see saveimport.h). */
#include <string.h>
#include "saveimport.h"

static const char DESMUME_END[] = "|-DESMUME SAVE-|";
static const char DESMUME_SNIP[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";

int save_convert(const uint8_t* in, size_t n, uint8_t* out) {
    size_t len = n;
    int desmume = 0;
    const size_t endlen = sizeof DESMUME_END - 1;
    if (n >= endlen && !memcmp(in + n - endlen, DESMUME_END, endlen)) {
        desmume = 1;
        /* the footer starts at its "snip" line; without it, the footer is 122 bytes */
        const size_t sl = sizeof DESMUME_SNIP - 1;
        len = n >= 122 ? n - 122 : 0;
        if (n >= sl && len + sl <= n && memcmp(in + len, DESMUME_SNIP, sl) != 0) {
            for (size_t i = 0; i + sl <= n; i++) if (!memcmp(in + i, DESMUME_SNIP, sl)) { len = i; break; }
        }
    }
    if (len > SAVE_SIZE) return SAVE_BAD_SIZE;
    if (len < SAVE_SIZE && !desmume) return SAVE_BAD_SIZE;       /* a raw file has to be whole */
    memcpy(out, in, len);
    memset(out + len, 0xFF, SAVE_SIZE - len);                    /* DeSmuME leaves out the trailing 0xFF bytes */

    if (memcmp(out, "DRAGON QUEST IX", 15) != 0) return SAVE_NOT_DQIX;
    size_t used = 0;
    for (size_t i = 0; i < SAVE_SIZE; i++) if (out[i] != 0x00 && out[i] != 0xFF) used++;
    if (used < 200) return SAVE_EMPTY;                           /* a new save has about 45 bytes; one with an adventure thousands */
    return SAVE_OK;
}

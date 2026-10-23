/* Playing with friends far away ("nearby DS systems" over the internet).
 *
 * The DS's local wireless needs answers within microseconds, which no
 * internet connection can give. So every PC in a session runs *every*
 * player's console - its own in the window, the others as hidden copies -
 * and the consoles talk to each other through air.c on that PC. Only the
 * buttons travel over the internet: each player's presses are used a few
 * frames later (the input delay) on all PCs at once. The game is
 * deterministic, so every PC computes exactly the same thing, wireless
 * included: tag mode, visiting and inviting, up to four players.
 *
 * One player hosts (others connect to them, TCP); everyone's save and
 * console settings go to everyone when the host starts, then all games boot
 * together. A player who leaves is switched off at the same frame on every
 * PC. Fingerprints of memory are compared now and then to catch PCs that
 * drift apart (a different build, say).
 *
 *   DQIX_SESSION=host[:port[:players]]     host (players: start automatically)
 *   DQIX_SESSION=join:address[:port]       join
 *   DQIX_SESSION=join:DQ-XXXX-XXXX         join with a code (through the relay, relay.h)
 *   DQIX_DELAY=frames                       input delay (default: from the pings)
 */
#pragma once
#include <stdint.h>

#define SESSION_PORT 27900
#define SESSION_MAX 4

int session_hidden(void);                 /* this process is another player's console */
int session_running(void);                /* inputs go through the session */
int session_console(void);                /* this console's number (0 = host) */
int session_players(void);
int session_requested(void);              /* DQIX_SESSION or hidden: wireless online is off */

/* main(): before firmware_init (session_pre_boot) and before the game boots
 * (session_boot: the lobby, then the start). */
void session_pre_boot(int argc, char** argv);
void session_boot(void);

/* input.c, once per frame: the input this console uses this frame. `local` =
 * what the player presses now (used `delay` frames later). */
uint32_t session_frame_input(uint64_t frame, uint32_t local);
uint32_t session_pack(uint16_t keys, int touch, int x, int y);
void session_unpack(uint32_t v, uint16_t* keys, int* touch, int* x, int* y);

/* lobby and status, for the window */
typedef struct {
    int state;                            /* 0 off, 1 connecting, 2 lobby, 3 running, 4 ended, 5 failed */
    int host;                             /* this PC hosts */
    int me, n, delay;
    char names[SESSION_MAX][32];
    int ping_ms[SESSION_MAX];
    int gone[SESSION_MAX];
    char address[128];                    /* what friends type in (host) / where we connect (guest) */
    char message[256];                    /* the latest news (joined, left, out of sync, errors) */
    char hint[1000];                      /* host: how friends get in */
    int ready;                            /* host: the address is known */
    int reach;                            /* host: 1 direct, 2 a relay code, 3 the home network only */
    int desync;
} SessionInfo;
void session_info(SessionInfo* out);
void session_start_now(void);             /* host: everyone is here */
void session_cancel(void);
void session_set_name(const char* name);
/* the lobby screen etc.: called repeatedly while the lobby waits */
extern void (*session_lobby_hook)(void);

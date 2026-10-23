/* The relay: for friends who can't connect directly (neither router lets
 * anyone in: apartment and mobile internet). Both games connect out to a
 * small service on the internet (tools/relay/worker.js) with a WebSocket,
 * which every network allows; a code ties host and friends together.
 *
 * The session doesn't know about it: on the host, each friend who arrives
 * through the relay becomes an ordinary TCP connection to the host's own
 * listening port (from 127.0.0.1); a friend connects to a port on their own
 * PC that leads to the relay. So the lobby, the start and the lockstep are
 * the same as over a direct connection. */
#pragma once
#include <stddef.h>

/* the relay's address: DQIX_RELAY (tests), else the one built in (empty = none yet) */
const char* relay_url(void);

/* a new code, "ABCD2345" (8 letters/digits, no 0/O/1/I/L); shown as DQ-ABCD-2345 */
void relay_new_code(char* out, size_t n);
/* "DQ-abcd-2345", "abcd 2345"...: the code in it (8 characters) or 0 if it isn't one */
int relay_parse_code(const char* text, char* out, size_t n);
void relay_show_code(const char* code, char* out, size_t n);   /* "DQ-ABCD-2345" */

/* host: friends arriving through the relay are connected to 127.0.0.1:port.
 * 1 = waiting at the relay; 0 = not possible (why says so). */
int relay_host_start(const char* code, int port, char* why, size_t wn);
/* friend: connects to the relay; *port gets a port on this PC to connect to
 * (once). 1 = the host is there; 0 = not (why says so: wrong code...). */
int relay_join_start(const char* code, int* port, char* why, size_t wn);
/* the relay connection broke (lobby: friends can't join through it any more) */
int relay_broken(void);

/* where to write what happens (session/front.log) */
void relay_set_log(void (*fn)(const char* line));

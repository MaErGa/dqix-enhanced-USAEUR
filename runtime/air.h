/* The air between DS consoles: lets the wireless chips of up to four copies
 * of the game (separate processes on one PC) hear each other, the way DSes
 * lying side by side do.
 *
 * Everything is deterministic. All consoles count time in the same 8 us
 * ticks since they were switched on together. A frame a console starts to
 * send at tick t reaches the others at tick t + preamble (>= 12 ticks), so a
 * console may run up to AIR_LOOKAHEAD ticks ahead of the slowest one before
 * it has to wait: whatever it receives at tick T has then certainly been
 * sent already. Frames from several consoles are taken in (tick, console)
 * order. The same inputs therefore give the same results on every PC, which
 * is what lets friends far apart each run all the consoles (session.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define AIR_MAX 4                 /* consoles */
#define AIR_LOOKAHEAD 11          /* ticks a console may run ahead of the others */
#define AIR_FRAME_MAX 2560        /* 12-byte TX header + 802.11 frame */

enum { AIR_FRAME = 0, AIR_REPLY = 1 };   /* REPLY: an MP reply, only for the console that sent the CMD */

typedef struct {
    uint64_t rx_tick;             /* when receivers start to receive it */
    uint32_t cmd_id;              /* REPLY: the CMD it answers; FRAME (CMD): its id */
    uint8_t kind, aid, from;
    uint16_t len;                 /* bytes in data (TX header + frame) */
    uint8_t data[AIR_FRAME_MAX];
} AirFrame;

/* DQIX_AIR="name,index,count" or session.c. Returns 1 if this console is
 * connected to the air. */
int air_init(const char* name, int index, int count);
int air_active(void);
int air_index(void);
int air_count(void);

/* Time: the tick this console is about to process (everything before it is
 * done and sent). `listening`: the radio is on, so wait until the others
 * have caught up enough for reception at `tick` to be complete. */
void air_reach(uint64_t tick, int listening);

/* Sending (from inside tick processing). */
void air_send(uint64_t rx_tick, int kind, int aid, uint32_t cmd_id, const uint8_t* data, int len);

/* The next frame from another console that starts at or before `now`
 * (REPLY frames are kept aside for air_take_reply). Returns 0 if none. */
int air_recv(uint64_t now, AirFrame* out);
/* Forget everything up to `now` (radio off). */
void air_skip(uint64_t now);
/* The reply from the client with association id `aid` to CMD `cmd_id`, if it
 * arrived by `now`. */
int air_take_reply(uint64_t now, uint32_t cmd_id, int aid, AirFrame* out);

/* ---- lockstep inputs (session.c): every console's buttons per frame, the
 * same on every PC. One ring per console, written by one process (the
 * player's own game for its console, the network for the others). */
#define AIR_INPUT_END 0xFFFFFFFFu  /* the console was switched off before this frame */
void air_input_put(int console, uint64_t frame, uint32_t v);        /* frames in order; gaps become "nothing pressed" */
uint64_t air_input_count(int console);                               /* frames written */
uint32_t air_input_get(int console, uint64_t frame);                 /* waits until it is there */
int air_input_peek(int console, uint64_t frame, uint32_t* v);        /* 0 if not there (yet / any more) */
/* a fingerprint of a console's memory at a frame, to notice if PCs drift apart */
void air_hash_put(int console, uint64_t frame, uint64_t hash);
int air_hash_get(int console, uint64_t frame, uint64_t* hash);

/* Called while waiting on other consoles (keeps a window responsive). */
extern void (*air_idle_hook)(void);
/* Another console stopped for good (session ended for it): stop waiting on it. */
void air_set_gone(int index);
void air_leave_now(void);                 /* this console stops: nobody waits for it */
uint64_t air_progress(void);              /* changes whenever any console gets further */
double air_waiting_ms(void);               /* how long this console has been waiting (0: it isn't) */
int air_wait_describe(char* buf, size_t n, int* console);   /* what for, in words */
void air_status(char* buf, size_t n);     /* every console's progress, one line */
void air_set_name(int console, const char* name);   /* for those descriptions */

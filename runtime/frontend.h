/* Frontend: what the player sees, hears and presses. Two implementations:
 * frontend_none.c (headless: scripted input, frame dumps, optional .wav) and
 * frontend_sdl.c (window, keyboard/mouse/gamepad, audio, frame pacing). */
#pragma once
#include <stdint.h>

void frontend_init(int argc, char** argv);
/* Called once per frame at VBlank, after g_screen holds the finished frame.
 * Presents it, collects input into g_keys_down / g_touch_*, plays audio and
 * paces the game to the DS frame rate. */
void frontend_frame(void);
void frontend_shutdown(void);

extern uint32_t g_screen[2][256 * 192];   /* 0 = top, 1 = bottom; 0x00RRGGBB */
extern uint16_t g_keys_down;              /* A B SELECT START RIGHT LEFT UP DOWN R L X Y (bits 0-11) */
extern int g_touch_down, g_touch_x, g_touch_y;
extern uint16_t g_fe_keys;                /* frontend's own input, merged by input_update() */
extern int g_fe_touch, g_fe_tx, g_fe_ty;
/* the gamepad's left stick (-32768..32767) and the D-pad directions it means
 * (kept apart: with "walk in any direction" on, the stick walks instead) */
extern int g_fe_stick_x, g_fe_stick_y;
extern int g_fe_rstick_x;                 /* the right stick, left/right: turns the camera (input.c) */
extern uint16_t g_fe_stick_keys;
extern int g_analog_walk;                 /* Modern > walk in any direction (settings.ini analog_walk) */

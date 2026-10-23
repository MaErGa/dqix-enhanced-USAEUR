/* What the 3D geometry engine (gpu3d.c) hands to a renderer: one frame's
 * polygons in screen space, in drawing order, plus texture decoding. Shared
 * by the software rasterizer (gpu3d.c) and the OpenGL renderer (gl_render.c). */
#pragma once
#include <stdint.h>

typedef struct {             /* screen-space vertex */
    float x, y;              /* pixels (256x192 space) */
    float zn;                /* z/w mapped to 0..1 */
    float iw;                /* 1/w */
    float r, g, b, s, t;     /* color 0..31, texel coordinates */
} SVtx;

#define MAX_POLY 2048
#define MAX_PV 10
typedef struct {
    SVtx v[MAX_PV];
    int n;
    uint32_t attr, teximg, pltt;
    int translucent;
    float ymin, ymax;
} Poly;

/* One frame to render: polygons sorted like the hardware draws them
 * (opaque first, then translucent), and the state latched at SWAP_BUFFERS. */
typedef struct {
    const Poly* const* order;
    int n, n_opaque;
    int wbuf;                /* depth from w instead of z */
    int width;               /* DS pixels across: 256, or more for widescreen (x of the vertices: 0..width) */
} Frame3D;

/* A renderer that replaces the software rasterizer (set by the frontend). */
typedef struct {
    void (*vblank)(void);                      /* every VBlank, before a possible render */
    void (*render)(const Frame3D* f);          /* a new frame after SWAP_BUFFERS */
} Renderer3D;
extern const Renderer3D* g_renderer3d;         /* NULL = software */
extern int g_renderer3d_verify;                /* also run the software renderer (testing) */
/* Widescreen (GPU renderer only): DS pixels added left and right of the 3D
 * picture; 0 = off. The game sees no difference. */
extern int g_wide_side;
/* Widescreen: the DS's box test (the game's "is it in view?") uses the wide
 * view too: DS pixels added each side, 0 = the DS's own view. */
extern int g_wide_cull;

/* Texture helpers for renderers. */
int  gpu3d_tex_size(uint32_t teximg, int* w, int* h);          /* 0 if no texture (format 0) */
void gpu3d_decode_texture(uint32_t teximg, uint32_t pltt, uint8_t* rgba);   /* w*h RGBA8 */
uint64_t gpu3d_texture_hash(uint32_t teximg, uint32_t pltt);   /* changes when its data/palette change */

/* Software renderer output (tests): one line, 6-bit colour | 5-bit alpha << 24. */
const uint32_t* gpu3d_soft_line(int line);

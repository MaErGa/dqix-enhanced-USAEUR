/* Standalone check of gpu3d.c: build a small scene through the geometry
 * ports and write the rendered frame to t3d.ppm. */
#include <stdio.h>
#include <string.h>
#include "nds.h"
uint8_t g_io[0x2000];
uint8_t* g_tex_slot[4];
uint8_t* g_texpal_slot[6];
Core g_c9;
uint64_t g_frame;
void irq_raise(Core* k, int b) { (void)k; (void)b; }
uint32_t g_3d_line32[256]; int g_3d_present;   /* 6-bit r | g << 8 | b << 16 | alpha << 24 */
void gpu3d_render_line(int line);
static void cmd(int c, const uint32_t* p, int n) {
    if (!n) { gx_write(0x440 + (c - 0x10) * 4, 0); return; }
    for (int i = 0; i < n; i++) gx_write(0x440 + (c - 0x10) * 4, p[i]);
}
#define C1(c, v) do { uint32_t _p = (v); cmd(c, &_p, 1); } while (0)
static uint32_t vtx_lo(int x, int y) { return (uint32_t)(x & 0xFFFF) | ((uint32_t)(y & 0xFFFF) << 16); }
int main(void) {
    gpu3d_init();
    *(uint16_t*)&g_io[0x60] = 0x0008;                 /* blending on */
    *(uint32_t*)&g_io[0x350] = 0x001F0000 | (4 << 10); /* clear: dark blue, opaque */
    *(uint16_t*)&g_io[0x354] = 0x7FFF;
    C1(0x10, 0); cmd(0x15, 0, 0);
    /* simple perspective: near 1, far 100, 90 degree fov -> x' = x, y' = y*4/3, w = -z */
    uint32_t proj[16] = { 0x1000,0,0,0, 0,0x1555,0,0, 0,0,(uint32_t)-0x1052,(uint32_t)-0x1000, 0,0,(uint32_t)-0x2052,0 };
    cmd(0x16, proj, 16);
    C1(0x10, 2); cmd(0x15, 0, 0);
    uint32_t tr[3] = { 0, 0, (uint32_t)(-3 * 4096) }; cmd(0x1C, tr, 3);
    C1(0x60, 0xBFFF0000);                             /* viewport: whole screen */
    C1(0x29, 0x001F00C0);                             /* alpha 31, render front+back */
    C1(0x40, 0);                                      /* triangles */
    C1(0x20, 0x001F); uint32_t v0[2] = { vtx_lo(-4096, -4096), 0 }; cmd(0x23, v0, 2);
    C1(0x20, 0x03E0); uint32_t v1[2] = { vtx_lo(4096, -4096), 0 }; cmd(0x23, v1, 2);
    C1(0x20, 0x7C00); uint32_t v2[2] = { vtx_lo(0, 4096), 0 }; cmd(0x23, v2, 2);
    cmd(0x41, 0, 0);
    /* translucent quad in front, front-face only */
    C1(0x29, 0x000F0080);
    C1(0x40, 1);
    C1(0x20, 0x7FFF);
    uint32_t q[4][2] = { { vtx_lo(-6000, -1000), 2048 }, { vtx_lo(-1000, -1000), 2048 }, { vtx_lo(-1000, 2000), 2048 }, { vtx_lo(-6000, 2000), 2048 } };
    for (int i = 0; i < 4; i++) cmd(0x23, q[i], 2);
    /* same quad wound the other way, offset right: culled if facing is right */
    uint32_t q2[4][2] = { { vtx_lo(1000, -1000), 2048 }, { vtx_lo(1000, 2000), 2048 }, { vtx_lo(6000, 2000), 2048 }, { vtx_lo(6000, -1000), 2048 } };
    for (int i = 0; i < 4; i++) cmd(0x23, q2[i], 2);
    cmd(0x41, 0, 0);
    C1(0x50, 0);
    gpu3d_vblank();
    gpu3d_render_start();
    FILE* f = fopen("t3d.ppm", "wb"); fprintf(f, "P6\n256 192\n255\n");
    for (int y = 0; y < 192; y++) { gpu3d_render_line(y); for (int x = 0; x < 256; x++) { uint32_t c = g_3d_line32[x]; uint8_t rgb[3] = { (uint8_t)((c & 63) << 2), (uint8_t)(((c >> 8) & 63) << 2), (uint8_t)(((c >> 16) & 63) << 2) }; fwrite(rgb, 1, 3, f); } }
    fclose(f);
    uint32_t st = gx_read(0x604); printf("polys %u verts %u\n", st & 0xFFF, st >> 16);
    return 0;
}

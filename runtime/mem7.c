/* ARM7 memory map. */
#include <string.h>
#include "nds.h"

uint8_t g_wram7[WRAM7_SIZE];

/* Shared WRAM as the ARM7 sees it, per WRAMCNT (0x04000247, set by the ARM9). */
static uint8_t* swram7(uint32_t a) {
    switch (g_io[0x247] & 3) {
    case 0: return &g_wram7[a & (WRAM7_SIZE - 1)];     /* all to ARM9: ARM7 sees its own WRAM */
    case 1: return &g_swram[0x4000 + (a & 0x3FFF)];   /* second half */
    case 2: return &g_swram[a & 0x3FFF];              /* first half */
    default: return &g_swram[a & (SWRAM_SIZE - 1)];    /* all to ARM7 */
    }
}

static inline uint8_t* resolve7(uint32_t a) {
    switch (a >> 24) {
    case 0x02: return &g_main_ram[a & (MAIN_RAM_SIZE - 1)];
    case 0x03: return a < 0x03800000 ? swram7(a) : &g_wram7[a & (WRAM7_SIZE - 1)];
    case 0x06: return vram7_ptr(a);
    default: return NULL;
    }
}

static uint32_t unmapped7(uint32_t a, int size) {
    if ((a >> 24) == 0x00 && a < 0x4000) return 0;              /* BIOS (calls are high-level) */
    if ((a >> 24) == 0x08 || (a >> 24) == 0x09 || (a >> 24) == 0x0A) return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    LOG_WARN("arm7 read%d from unmapped %08x\n", size * 8, a);
    return 0;
}

uint8_t mem7_read8(uint32_t a) {
    if ((a >> 24) == 4) return (uint8_t)io7_read(a, 1);
    uint8_t* p = resolve7(a);
    return p ? *p : (uint8_t)unmapped7(a, 1);
}
uint16_t mem7_read16(uint32_t a) {
    a &= ~1u;
    if ((a >> 24) == 4) return (uint16_t)io7_read(a, 2);
    uint8_t* p = resolve7(a);
    if (!p) return (uint16_t)unmapped7(a, 2);
    uint16_t v; memcpy(&v, p, 2); return v;
}
uint32_t mem7_read32(uint32_t a) {
    a &= ~3u;
    if ((a >> 24) == 4) return io7_read(a, 4);
    uint8_t* p = resolve7(a);
    if (!p) return unmapped7(a, 4);
    uint32_t v; memcpy(&v, p, 4); return v;
}
/* Writes to the BIOS (0-3FFF) do nothing on the DS. The SDK's wireless code
 * writes there through an unset pointer while connected; log the first only. */
static int bios_write(uint32_t a) {
    static int said;
    if (a >= 0x4000) return 0;
    if (!said) { said = 1; LOG_WARN("arm7 writes to the BIOS at %08x (ignored, as on the DS; not logged again)\n", a); }
    return 1;
}
void mem7_write8(uint32_t a, uint8_t v) {
    if ((a >> 24) == 4) { io7_write(a, v, 1); return; }
    uint8_t* p = resolve7(a);
    if (p) *p = v; else if (!bios_write(a)) LOG_WARN("arm7 write8 to unmapped %08x = %02x\n", a, v);
}
void mem7_write16(uint32_t a, uint16_t v) {
    a &= ~1u;
    if ((a >> 24) == 4) { io7_write(a, v, 2); return; }
    uint8_t* p = resolve7(a);
    if (p) memcpy(p, &v, 2); else if (!bios_write(a)) LOG_WARN("arm7 write16 to unmapped %08x = %04x\n", a, v);
}
void mem7_write32(uint32_t a, uint32_t v) {
    a &= ~3u;
    if ((a >> 24) == 4) { io7_write(a, v, 4); return; }
    uint8_t* p = resolve7(a);
    if (p) memcpy(p, &v, 4); else if (!bios_write(a)) LOG_WARN("arm7 write32 to unmapped %08x = %08x\n", a, v);
}

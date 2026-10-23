/* DQIX PC runtime entry point: load the ROM, set up the machine the way the
 * DS firmware leaves it, and run the recompiled game on both CPUs. */
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "frontend.h"
#include "dispatch.h"
#include "session.h"

uint8_t* g_rom;
size_t g_rom_size;
extern unsigned long long g_calls_traced;

static void load_rom(const char* path) {
    const uint8_t* shared; size_t n;
    if (rt_host_rom(&shared, &n)) { g_rom = (uint8_t*)shared; g_rom_size = n; return; }   /* hosted: the host's copy */
    FILE* f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open ROM %s\n", path);
    fseek(f, 0, SEEK_END);
    g_rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_rom = malloc(g_rom_size);
    if (fread(g_rom, 1, g_rom_size, f) != g_rom_size) rt_fatal("short read on ROM\n");
    fclose(f);
}

static uint32_t rom32(uint32_t off) { uint32_t v; memcpy(&v, g_rom + off, 4); return v; }
static void ram32(uint32_t a, uint32_t v) { memcpy(&g_main_ram[a & (MAIN_RAM_SIZE - 1)], &v, 4); }
static void ram16(uint32_t a, uint16_t v) { memcpy(&g_main_ram[a & (MAIN_RAM_SIZE - 1)], &v, 2); }
static void ram8(uint32_t a, uint8_t v) { g_main_ram[a & (MAIN_RAM_SIZE - 1)] = v; }

/* What the BIOS + firmware leave behind when they start a game card. */
static void boot_firmware(void) {
    uint32_t a9_rom = rom32(0x20), a9_load = rom32(0x28), a9_size = rom32(0x2C);
    uint32_t a7_rom = rom32(0x30), a7_load = rom32(0x38), a7_size = rom32(0x3C);
    memcpy(&g_main_ram[a9_load & (MAIN_RAM_SIZE - 1)], g_rom + a9_rom, a9_size);
    memcpy(&g_main_ram[a7_load & (MAIN_RAM_SIZE - 1)], g_rom + a7_rom, a7_size);
    memcpy(&g_main_ram[0x027FFE00 & (MAIN_RAM_SIZE - 1)], g_rom, 0x170);   /* cartridge header */
    uint32_t chip = 0x00003FC2;
    ram32(0x027FF800, chip); ram32(0x027FF804, chip); ram32(0x027FFC00, chip); ram32(0x027FFC04, chip);
    ram16(0x027FF808, (uint16_t)rom32(0x15E)); ram16(0x027FFC08, (uint16_t)rom32(0x15E));
    ram16(0x027FF80A, (uint16_t)rom32(0x06C)); ram16(0x027FFC0A, (uint16_t)rom32(0x06C));
    ram16(0x027FFC10, 0x5835);
    ram16(0x027FFC40, 1);                    /* booted from a card */
    /* what the firmware leaves in RAM: the user settings from the flash
     * (io7.c builds the image) and where they live */
    ram32(0x027FF864, 0);
    ram32(0x027FF868, firmware_user_offset());
    const uint8_t* us = firmware_user_settings();
    for (int i = 0; i < 0x70; i++) ram8(0x027FFC80 + i, us[i]);
    g_io[0x247] = 3;                         /* WRAMCNT: all shared WRAM to the ARM7 */
    g_io[0x300] = 1; g_io7[0x300] = 1;       /* POSTFLG */
    /* ARM9: system mode, IRQs masked */
    g_arm9.r[13] = 0x023FFE00;
    g_arm9.cpsr_ctl = 0xDF;
    /* ARM7: system mode with the BIOS's stacks in ARM7 WRAM */
    g_arm7.cpsr_ctl = 0xDF;
    g_arm7.r[13] = 0x0380FD80;
    g_arm7.bank_r13[2] = 0x0380FF80;         /* IRQ */
    g_arm7.bank_r13[3] = 0x0380FFC0;         /* SVC */
    LOG_INFO("ARM9: %u bytes at %08x, entry %08x; ARM7: %u bytes at %08x, entry %08x\n",
             a9_size, a9_load, rom32(0x24), a7_size, a7_load, rom32(0x34));
}

void prof_report(FILE* f);
void threads_dump(FILE* f);
static void report(void) {
    backup_flush();
    fprintf(stderr, "\n--- stopped after %llu frames, %u thread switches, %llu function calls traced ---\n",
            (unsigned long long)g_frame, g_thread_switches, g_calls_traced);
    trace_dump(stderr, 40);
    prof_report(stderr);
    if (getenv("DQIX_THREADS")) threads_dump(stderr);
}

char** g_argv;
int dqix_main(int argc, char** argv) {
    g_argv = argv;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom.nds> <extract/usa dir> [frames] [log level] [save file]\n", argv[0]);
        return 2;
    }
    load_rom(argv[1]);
    if (argc > 3) g_frame_limit = strtoull(argv[3], NULL, 0);
    if (argc > 4) g_log_level = atoi(argv[4]);
    backup_init(argc > 5 ? argv[5] : "dqix.sav");
    atexit(report);
    frontend_init(argc, argv);
    rtc_base();                              /* fix the clock start now (reports replay from it) */
    dispatch_init();
    overlays_init(argv[2]);
    session_pre_boot(argc, argv);            /* a session: wireless online off; another player's console */
    firmware_init();
    session_boot();                          /* a session: the lobby, then everyone's game starts together */
    boot_firmware();
    vram_remap();
    threads_init();
    threads_start_arm7(rom32(0x34));
    dispatch_call(&g_arm9, rom32(0x24), 0);
    LOG_WARN("the game's entry function returned\n");
    return 0;
}

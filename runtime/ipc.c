/* IPC between the two CPUs: the sync register and the two 16-word FIFOs. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "nds.h"

typedef struct { uint32_t q[16]; int head, count; } Fifo;
static Fifo g_fifo[2];             /* g_fifo[i] = words sent by CPU i */
static uint16_t g_sync_out[2];     /* bits 8-11 each CPU shows the other */
static uint16_t g_sync_irqen[2];   /* bit 14 */
static uint16_t g_cnt[2];          /* FIFOCNT enable bits (2, 10, 15) */
static int g_err[2];
static uint32_t g_last[2];

static Core* other(Core* k) { return k->id == 0 ? &g_c7 : &g_c9; }

uint16_t ipc_sync_read(Core* k) {
    return (uint16_t)(((g_sync_out[other(k)->id] >> 8) & 0xF) | g_sync_out[k->id] | g_sync_irqen[k->id]);
}

void ipc_sync_write(Core* k, uint16_t v) {
    g_sync_out[k->id] = v & 0x0F00;
    g_sync_irqen[k->id] = v & 0x4000;
    if ((v & 0x2000) && g_sync_irqen[other(k)->id]) irq_raise(other(k), IRQ_IPCSYNC);
}

uint16_t ipc_fifocnt_read(Core* k) {
    Fifo* tx = &g_fifo[k->id];
    Fifo* rx = &g_fifo[other(k)->id];
    uint16_t v = g_cnt[k->id];
    if (tx->count == 0) v |= 0x0001;
    if (tx->count == 16) v |= 0x0002;
    if (rx->count == 0) v |= 0x0100;
    if (rx->count == 16) v |= 0x0200;
    if (g_err[k->id]) v |= 0x4000;
    return v;
}

void ipc_fifocnt_write(Core* k, uint16_t v) {
    Fifo* tx = &g_fifo[k->id];
    Fifo* rx = &g_fifo[other(k)->id];
    uint16_t old = g_cnt[k->id];
    if (v & 0x0008) { tx->count = 0; tx->head = 0; }
    if (v & 0x4000) g_err[k->id] = 0;
    g_cnt[k->id] = v & 0x8404;
    if (!(old & 0x0004) && (v & 0x0004) && tx->count == 0) irq_raise(k, IRQ_IPC_SEND_EMPTY);
    if (!(old & 0x0400) && (v & 0x0400) && rx->count) irq_raise(k, IRQ_IPC_RECV_NOT_EMPTY);
}

void ipc_fifo_send(Core* k, uint32_t v) {
    if (!(g_cnt[k->id] & 0x8000)) return;
    Fifo* tx = &g_fifo[k->id];
    if (tx->count == 16) { g_err[k->id] = 1; return; }
    tx->q[(tx->head + tx->count++) % 16] = v;
    LOG_DEBUG("pxi: %s sends tag %u data %07x\n", k->name, v & 0x1F, v >> 6);
    { static int lg = -1; if (lg < 0) lg = getenv("DQIX_PXILOG") != NULL;
      extern uint32_t threads_cur_ctx9(void);
      if (lg) fprintf(stderr, "pxi f%llu %s tag %2u err %u data %07x thr %08x\n", (unsigned long long)g_frame, k->name, v & 0x1F, (v >> 5) & 1, v >> 6, k->id == 0 ? threads_cur_ctx9() : 0); }
    if (k->id == 0 && (v & 0x1F) == 7) { extern void music_pxi(uint32_t); music_pxi(v >> 6); }   /* sound commands: music packs (music.c) */
    if (tx->count == 1 && (g_cnt[other(k)->id] & 0x0400)) irq_raise(other(k), IRQ_IPC_RECV_NOT_EMPTY);
}

uint32_t ipc_fifo_recv(Core* k) {
    if (!(g_cnt[k->id] & 0x8000)) return g_last[k->id];
    Fifo* rx = &g_fifo[other(k)->id];
    if (!rx->count) { g_err[k->id] = 1; return g_last[k->id]; }
    uint32_t v = rx->q[rx->head];
    rx->head = (rx->head + 1) % 16;
    rx->count--;
    g_last[k->id] = v;
    if (rx->count == 0 && (g_cnt[other(k)->id] & 0x0004)) irq_raise(other(k), IRQ_IPC_SEND_EMPTY);
    return v;
}
void ipc_dump(FILE* f) {
    fprintf(f, "ipc: from ARM9 count %d, from ARM7 count %d (head word %08x), cnt9 %04x cnt7 %04x err %d/%d\n", g_fifo[0].count, g_fifo[1].count,
            g_fifo[1].count ? g_fifo[1].q[g_fifo[1].head] : 0, g_cnt[0], g_cnt[1], g_err[0], g_err[1]);
}

/* The DS wireless chip (ARM7 0x04800000-0x0480FFFF) and a virtual access
 * point it can connect to.
 *
 * A C translation of melonDS's Wifi.cpp / WifiAP.cpp (GPL-3.0-or-later,
 * Copyright 2016-2026 melonDS team), reduced to what a game needs to go
 * online through an access point: registers, wifi RAM, the microsecond
 * counter and its interrupts, transceiver power, TX slots, RX into the ring
 * buffer, baseband/RF chips (channel selection), and local multiplayer:
 * frames between consoles and the MP CMD / REPLY / ACK exchanges, through
 * the air shared with other copies of the game (air.c; deterministic: all
 * consoles count the same 8 us ticks and a frame carries the tick at which
 * it arrives).
 *
 * The access point answers probe, auth and association requests, sends a
 * beacon every 128 ms, and passes data frames to and from net.c as Ethernet
 * frames. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include "air.h"

/* registers */
enum {
    W_ID = 0x000, W_ModeReset = 0x004, W_ModeWEP = 0x006, W_TXStatCnt = 0x008, W_IF = 0x010, W_IE = 0x012,
    W_MACAddr0 = 0x018, W_MACAddr1 = 0x01A, W_MACAddr2 = 0x01C, W_BSSID0 = 0x020, W_BSSID1 = 0x022, W_BSSID2 = 0x024,
    W_AIDLow = 0x028, W_AIDFull = 0x02A, W_TXRetryLimit = 0x02C, W_RXCnt = 0x030, W_WEPCnt = 0x032,
    W_TRXPower = 0x034, W_PowerUS = 0x036, W_PowerTX = 0x038, W_PowerState = 0x03C, W_PowerForce = 0x040,
    W_Random = 0x044, W_PowerDownCtrl = 0x048,
    W_RXBufBegin = 0x050, W_RXBufEnd = 0x052, W_RXBufWriteCursor = 0x054, W_RXBufWriteAddr = 0x056,
    W_RXBufReadAddr = 0x058, W_RXBufReadCursor = 0x05A, W_RXBufCount = 0x05C, W_RXBufDataRead = 0x060,
    W_RXBufGapAddr = 0x062, W_RXBufGapSize = 0x064,
    W_TXBufWriteAddr = 0x068, W_TXBufCount = 0x06C, W_TXBufDataWrite = 0x070, W_TXBufGapAddr = 0x074, W_TXBufGapSize = 0x076,
    W_TXSlotBeacon = 0x080, W_TXBeaconTIM = 0x084, W_ListenCount = 0x088, W_BeaconInterval = 0x08C, W_ListenInterval = 0x08E,
    W_TXSlotCmd = 0x090, W_TXSlotReply1 = 0x094, W_TXSlotReply2 = 0x098, W_TXSlotLoc1 = 0x0A0, W_TXSlotLoc2 = 0x0A4,
    W_TXSlotLoc3 = 0x0A8, W_TXReqReset = 0x0AC, W_TXReqSet = 0x0AE, W_TXReqRead = 0x0B0, W_TXSlotReset = 0x0B4,
    W_TXBusy = 0x0B6, W_TXStat = 0x0B8, W_Preamble = 0x0BC, W_CmdTotalTime = 0x0C0, W_CmdReplyTime = 0x0C4,
    W_RXFilter = 0x0D0, W_RXLenCrop = 0x0DA, W_RXFilter2 = 0x0E0,
    W_USCountCnt = 0x0E8, W_USCompareCnt = 0x0EA, W_CmdCountCnt = 0x0EE,
    W_USCompare0 = 0x0F0, W_USCompare1 = 0x0F2, W_USCompare2 = 0x0F4, W_USCompare3 = 0x0F6,
    W_USCount0 = 0x0F8, W_USCount1 = 0x0FA, W_USCount2 = 0x0FC, W_USCount3 = 0x0FE,
    W_ContentFree = 0x10C, W_PreBeacon = 0x110, W_CmdCount = 0x118, W_BeaconCount1 = 0x11C, W_BeaconCount2 = 0x134,
    W_BBCnt = 0x158, W_BBWrite = 0x15A, W_BBRead = 0x15C, W_BBBusy = 0x15E, W_BBMode = 0x160, W_BBPower = 0x168,
    W_RFData2 = 0x17C, W_RFData1 = 0x17E, W_RFBusy = 0x180, W_RFCnt = 0x184,
    W_TXHeaderCnt = 0x194, W_RFPins = 0x19C, W_RXStatIncIF = 0x1A8, W_RXStatIncIE = 0x1AA,
    W_RXStatHalfIF = 0x1AC, W_RXStatHalfIE = 0x1AE, W_TXErrorCount = 0x1C0, W_RXCount = 0x1C4,
    W_CMDStat0 = 0x1D0, W_TXSeqNo = 0x210, W_RFStatus = 0x214, W_IFSet = 0x21C, W_RXTXAddr = 0x268,
};

#define TICK 8                                   /* the chip is stepped every 8 microseconds */
#define TIME_CHECK_MASK (~(uint32_t)(TICK - 1))
#define IOPORT(x) w_io[(x) >> 1]
#define IOPORT8(x) ((uint8_t*)w_io)[x]
#define RAM16(a) (*(uint16_t*)&g_ram[(a)])

typedef struct { int valid, sent; uint16_t addr, length; uint8_t rate, phase; int phase_time; uint32_t halfword_mask; } TXSlot;

static uint8_t g_ram[0x2000];
static uint16_t w_io[0x1000 >> 1];
static int g_enabled, g_power_on;
static uint16_t g_random;
static uint64_t g_us_timestamp, g_us_counter, g_us_compare;
static int g_block_beacon_irq14;
static uint32_t g_cmd_counter;
#define POST_BEACON_EXTRA_MS 24
static uint8_t g_bb[0x100], g_bb_ro[0x100];
static uint8_t g_rf_version;
static uint32_t g_rf[0x40];
static uint32_t g_rf_chan_index[2], g_rf_chan_data[14][2];
static int g_channel;
static TXSlot g_tx[6];
/* local multiplayer */
static const uint8_t MP_CMD_MAC[6] = { 0x03, 0x09, 0xBF, 0x00, 0x00, 0x00 };
static const uint8_t MP_REPLY_MAC[6] = { 0x03, 0x09, 0xBF, 0x00, 0x00, 0x10 };
static const uint8_t MP_ACK_MAC[6] = { 0x03, 0x09, 0xBF, 0x00, 0x00, 0x03 };
static uint64_t g_tick;                           /* 8 us ticks since the DS was switched on (all consoles agree) */
static int g_mp_reply_timer;
static uint16_t g_mp_client_mask, g_mp_client_fail, g_mp_last_seqno = 0xFFFF;
static uint32_t g_cmd_seq, g_cmd_id;              /* the CMD being sent (host) */
static uint32_t g_rx_cmd_id;                      /* air id of the frame being received */
static uint8_t g_txbuf[0x2000];
static uint8_t g_rxbuf[2048];
static uint32_t g_rxbuf_ptr;
static int g_rx_time;
static uint32_t g_rx_halfword_mask;
static uint32_t g_com;                           /* bit 0 receiving, bit 1 sending */
static int g_tx_cur;
static uint32_t g_rx_counter;
static int g_us_until_power_on;
static uint32_t g_cycle_acc;                     /* bus cycles not yet turned into ticks */

static int g_log = -1;
#define WLOG(...) do { if (g_log < 0) g_log = getenv("DQIX_WIFILOG") != NULL; if (g_log) fprintf(stderr, __VA_ARGS__); } while (0)

static int mac_eq(const uint8_t* a, const uint8_t* b) { return !memcmp(a, b, 6); }
static int mac_bcast(const uint8_t* a) { static const uint8_t f[6] = { 255, 255, 255, 255, 255, 255 }; return !memcmp(a, f, 6); }

/* ================================================================ access point */
static const char AP_NAME[] = "DQIX-PC";
static const uint8_t AP_MAC[6] = { 0x00, 0xF0, 0x77, 0x77, 0x77, 0x77 };
#define AP_CHANNEL 6
static uint64_t ap_us;
static uint16_t ap_seq;
static int ap_beacon_due, ap_client;              /* client: 0 none, 1 authenticated, 2 associated */
static uint8_t ap_pkt[2048]; static int ap_pkt_len, ap_rx_num;
static uint8_t ap_lan[2048];

int net_send(const uint8_t* eth, int len);       /* net.c: an Ethernet frame from the DS */
int net_recv(uint8_t* eth);                      /* net.c: the next Ethernet frame for the DS (0 if none) */

static uint8_t* put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }
static uint8_t* put32(uint8_t* p, uint32_t v) { p = put16(p, (uint16_t)v); return put16(p, (uint16_t)(v >> 16)); }
static uint8_t* put64(uint8_t* p, uint64_t v) { p = put32(p, (uint32_t)v); return put32(p, (uint32_t)(v >> 32)); }
static uint8_t* putmac(uint8_t* p, const uint8_t* m) { memcpy(p, m, 6); return p + 6; }
static uint8_t* putseq(uint8_t* p) { p = put16(p, ap_seq); ap_seq += 0x10; return p; }
static uint8_t* put_rates_ssid(uint8_t* p, int tim) {
    *p++ = 0x01; *p++ = 0x02; *p++ = 0x82; *p++ = 0x84;          /* rates */
    *p++ = 0x03; *p++ = 0x01; *p++ = AP_CHANNEL;                  /* channel */
    if (tim) { *p++ = 0x05; *p++ = 0x04; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; }
    *p++ = 0x00; *p++ = (uint8_t)strlen(AP_NAME);
    memcpy(p, AP_NAME, strlen(AP_NAME)); return p + strlen(AP_NAME);
}

static void ap_reset(void) { ap_us = 0x428888000ULL; ap_seq = 0x0120; ap_beacon_due = 0; ap_pkt_len = ap_rx_num = 0; ap_client = 0; }
static void ap_ms_timer(void) { ap_us += 0x400; if (!((uint32_t)ap_us & 0x1FC00)) ap_beacon_due = 1; }

static void ap_management(const uint8_t* d) {
    if (ap_rx_num) return;
    uint16_t fc = (uint16_t)(d[0] | d[1] << 8);
    uint8_t* p = ap_pkt;
    int sub = (fc >> 4) & 0xF;
    if (sub != 0x4 && !mac_eq(&d[16], AP_MAC)) return;            /* other than probes: must be for us */
    switch (sub) {
    case 0x0:                                                     /* association request */
        if (ap_client != 1) return;
        ap_client = 2; report_log_line("wifi: the DS joined the access point DQIX-PC\n"); WLOG("wifi AP: client associated\n");
        p = put16(p, 0x0010); p = put16(p, 0); p = putmac(p, &d[10]); p = putmac(p, AP_MAC); p = putmac(p, AP_MAC); p = putseq(p);
        p = put16(p, 0x0021); p = put16(p, 0); p = put16(p, 0xC001);
        *p++ = 0x01; *p++ = 0x02; *p++ = 0x82; *p++ = 0x84;
        break;
    case 0x4:                                                     /* probe request */
        p = put16(p, 0x0050); p = put16(p, 0); p = putmac(p, &d[10]); p = putmac(p, AP_MAC); p = putmac(p, AP_MAC); p = putseq(p);
        p = put64(p, ap_us); p = put16(p, 128); p = put16(p, 0x0021);
        p = put_rates_ssid(p, 0);
        break;
    case 0xA:                                                     /* disassociation */
        ap_client = 1;
        p = put16(p, 0x00A0); p = put16(p, 0); p = putmac(p, &d[10]); p = putmac(p, AP_MAC); p = putmac(p, AP_MAC); p = putseq(p);
        p = put16(p, 3);
        break;
    case 0xB:                                                     /* authentication (open) */
        ap_client = 1; WLOG("wifi AP: client authenticated\n");
        p = put16(p, 0x00B0); p = put16(p, 0); p = putmac(p, &d[10]); p = putmac(p, AP_MAC); p = putmac(p, AP_MAC); p = putseq(p);
        p = put16(p, 0); p = put16(p, 2); p = put16(p, 0);
        break;
    case 0xC:                                                     /* deauthentication */
        ap_client = 0;
        p = put16(p, 0x00C0); p = put16(p, 0); p = putmac(p, &d[10]); p = putmac(p, AP_MAC); p = putmac(p, AP_MAC); p = putseq(p);
        p = put16(p, 3);
        break;
    default: return;
    }
    ap_pkt_len = (int)(p - ap_pkt);
    ap_rx_num = 1;
}

/* a frame the DS transmitted (12-byte TX header + 802.11 frame) */
static void ap_send(const uint8_t* frame, int len) {
    if (frame[9] != AP_CHANNEL) return;
    const uint8_t* d = frame + 12;
    uint16_t fc = (uint16_t)(d[0] | d[1] << 8);
    int type = (fc >> 2) & 3;
    if (type == 0) { ap_management(d); return; }
    if (type != 2 || (fc & 0x0300) != 0x0100) return;            /* data, to the distribution system */
    if (!(d[24] == 0xAA && d[25] == 0xAA && d[26] == 0x03 && d[27] == 0 && d[28] == 0 && d[29] == 0)) return;   /* LLC/SNAP */
    if (ap_client != 2) return;
    int lan_len = (len - 30 - 4) + 14;
    if (lan_len < 14 || lan_len > (int)sizeof ap_lan) return;
    memcpy(&ap_lan[0], &d[16], 6);                                 /* destination */
    memcpy(&ap_lan[6], &d[10], 6);                                 /* source */
    memcpy(&ap_lan[12], &d[30], 2);                                /* ethertype */
    memcpy(&ap_lan[14], &d[32], (size_t)(lan_len - 14));
    net_send(ap_lan, lan_len);
}

static int ap_finish(uint8_t* data, uint8_t* p) {
    uint8_t* base = data + 12;
    while ((p - base) & 3) *p++ = 0xFF;
    p = put32(p, 0xDEADBEEF);                                      /* FCS (not checked) */
    int len = (int)(p - base);
    memset(data, 0, 8); data[8] = 20; data[9] = AP_CHANNEL; put16(data + 10, (uint16_t)len);
    return len + 12;
}

/* a frame for the DS to receive, or 0 */
static int ap_recv(uint8_t* data) {
    uint8_t* p = data + 12;
    if (ap_beacon_due) {
        static const uint8_t bc[6] = { 255, 255, 255, 255, 255, 255 };
        ap_beacon_due = 0;
        p = put16(p, 0x0080); p = put16(p, 0); p = putmac(p, bc); p = putmac(p, AP_MAC); p = putmac(p, AP_MAC); p = putseq(p);
        p = put64(p, ap_us); p = put16(p, 128); p = put16(p, 0x0021);
        p = put_rates_ssid(p, 1);
        return ap_finish(data, p);
    }
    if (ap_rx_num) {
        ap_rx_num = 0;
        memcpy(p, ap_pkt, (size_t)ap_pkt_len);
        return ap_finish(data, p + ap_pkt_len);
    }
    if (ap_client < 2) return 0;
    for (;;) {
        int n = net_recv(ap_lan);
        if (n <= 14) return 0;
        const uint8_t* mac = (const uint8_t*)&IOPORT(W_MACAddr0);
        if (!mac_bcast(&ap_lan[0]) && !mac_eq(&ap_lan[0], mac)) continue;
        if (mac_eq(&ap_lan[6], mac)) continue;
        p = put16(p, 0x0208); p = put16(p, 0); p = putmac(p, &ap_lan[0]); p = putmac(p, AP_MAC); p = putmac(p, &ap_lan[6]); p = putseq(p);
        *p++ = 0xAA; *p++ = 0xAA; *p++ = 0x03; *p++ = 0; *p++ = 0; *p++ = 0;
        memcpy(p, &ap_lan[12], 2); p += 2;
        memcpy(p, &ap_lan[14], (size_t)(n - 14)); p += n - 14;
        return ap_finish(data, p);
    }
}

/* ================================================================ chip */
static void check_irq(uint16_t old) {
    uint16_t now = IOPORT(W_IF) & IOPORT(W_IE);
    if (!old && now) irq_raise(&g_c7, IRQ_WIFI);
}
static void set_irq(int n) { uint16_t old = IOPORT(W_IF) & IOPORT(W_IE); IOPORT(W_IF) |= (uint16_t)(1 << n); check_irq(old); }

static void set_status(int st) {
    static const uint16_t pins[10] = { 0x04, 0x84, 0, 0x46, 0, 0x84, 0x87, 0, 0x46, 0x04 };
    IOPORT(W_RFStatus) = (uint16_t)st; IOPORT(W_RFPins) = pins[st];
}

static void update_power_status(int power) {                       /* 1 on, 0 no change, -1 off */
    int cur = 0;
    if (IOPORT(W_TRXPower) == 1) cur |= 1;
    if (!(IOPORT(W_PowerState) & (1 << 9))) cur |= 2;
    int req = cur;
    if (IOPORT(W_PowerForce) & (1 << 15)) req = (IOPORT(W_PowerForce) & 1) ? 0 : 3;
    else if (!(IOPORT(W_ModeReset) & 1)) req = 0;
    else {
        if (power == 0) {
            if ((IOPORT(W_PowerState) & 0x0202) == 0x0202) power = 1;
            else if ((IOPORT(W_PowerState) & 0x0201) == 0x0001) power = -1;
        }
        if (power == -1 && (IOPORT(W_PowerDownCtrl) & 1)) power = 0;
        if (power == 1) req = 3;
        else if (power == -1) req = IOPORT(W_PowerDownCtrl) ? 3 : 0;
        else if (IOPORT(W_PowerDownCtrl) & 2) req = 3;
    }
    if (req == cur) return;
    if (req & 1) { if (!(cur & 1)) { IOPORT(W_TRXPower) = 1; set_status(1); } }
    else { IOPORT(W_TRXPower) = 2; if (!g_com) { IOPORT(W_TRXPower) = 0; set_status(9); } }
    if (req & 2) {
        IOPORT(W_PowerState) |= 1 << 8;
        if (!(cur & 2) && g_us_until_power_on == 0) { g_us_until_power_on = -2048; set_irq(11); }
    } else {
        IOPORT(W_PowerState) &= (uint16_t)~((1 << 0) | (1 << 8));
        IOPORT(W_PowerState) |= 1 << 9;
        g_us_until_power_on = 0;
    }
}

static void set_irq13(void) {
    set_irq(13);
    if ((IOPORT(W_ModeWEP) & 7) == 0 && !(IOPORT(W_PowerTX) & 2)) update_power_status(-1);
}
static void start_tx_beacon(void);
static void set_irq14(int source) {                               /* 0 US compare, 1 beacon count, 2 forced */
    if (source != 2) IOPORT(W_BeaconCount1) = IOPORT(W_BeaconInterval);
    if (g_block_beacon_irq14 && source == 1) return;
    if (!(IOPORT(W_USCompareCnt) & 1)) return;
    set_irq(14);
    IOPORT(W_BeaconCount2) = 0xFFFF;
    IOPORT(W_TXReqRead) &= 0xFFF2;
    if (IOPORT(W_TXSlotBeacon) & 0x8000) start_tx_beacon();
    if (IOPORT(W_ListenCount) == 0) IOPORT(W_ListenCount) = IOPORT(W_ListenInterval);
    IOPORT(W_ListenCount)--;
}
static void set_irq15(void) { set_irq(15); if (IOPORT(W_PowerTX) & 1) update_power_status(1); }

static int preamble_len(int rate) { if (rate == 1) return 192; return (IOPORT(W_Preamble) & 4) ? 96 : 192; }
static int ticks_for(int us) { return (us + TICK - 1) / TICK; }
static int num_clients(uint16_t mask) { int n = 0; for (int i = 1; i < 16; i++) if (mask & (1 << i)) n++; return n; }
static void inc_tx_count(const TXSlot* s) { uint8_t n = g_ram[s->addr + 4]; if (n < 0xFF) n++; RAM16(s->addr + 4) = n; }

/* A frame the DS starts to send: other consoles receive it once the
 * preamble is over. */
static void air_out(const uint8_t* frame, int len, int kind, uint32_t cmd_id, int preamble_us) {
    if (!air_active()) return;
    WLOG("wifi: tick %llu send %s ch %d fc %04x len %d to %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned long long)g_tick, kind == AIR_REPLY ? "reply" : "frame",
         frame[9], frame[12] | frame[13] << 8, len, frame[16], frame[17], frame[18], frame[19], frame[20], frame[21]);
    air_send(g_tick + (uint64_t)ticks_for(preamble_us), kind, IOPORT(W_AIDLow), cmd_id, frame, len);
}

static void tx_send_frame(TXSlot* s, int num) {
    int noseq = 0;
    if (g_ram[s->addr + 4]) noseq = 2;
    else if (num == 1) noseq = (IOPORT(W_TXSlotCmd) & 0x4000) ? 1 : 0;
    if (!noseq) {
        if (!(IOPORT(W_TXHeaderCnt) & 4)) RAM16(s->addr + 0xC + 22) = (uint16_t)(IOPORT(W_TXSeqNo) << 4);
        IOPORT(W_TXSeqNo) = (IOPORT(W_TXSeqNo) + 1) & 0x0FFF;
    }
    uint16_t fc = RAM16(s->addr + 0xC);
    if ((fc & (1 << 14)) && (IOPORT(W_WEPCnt) & (1 << 15))) {
        uint32_t fcs = (s->addr + 0xC + s->length - 7) & ~1u;
        memcpy(&g_ram[fcs & 0x1FFC], "\x66\x44\x33\x22", 4);
    }
    int len = s->length;
    if (s->addr + len > 0x1FF4) len = 0x1FF4 - s->addr;
    memcpy(g_txbuf, &g_ram[s->addr], (size_t)(12 + len));
    if (noseq == 2) g_txbuf[0xD] |= 1 << 3;                        /* retry bit */
    if (g_channel == 0) return;
    g_txbuf[9] = (uint8_t)g_channel;
    int pre = preamble_len(s->rate);
    switch (num) {
    case 0: case 2: case 3:
        air_out(g_txbuf, 12 + len, AIR_FRAME, 0, pre);
        ap_send(g_txbuf, 12 + len);
        break;
    case 1:                                                       /* MP CMD: which clients should answer */
        g_txbuf[12 + 24 + 2] = (uint8_t)g_mp_client_mask; g_txbuf[12 + 24 + 3] = (uint8_t)(g_mp_client_mask >> 8);
        g_cmd_id = ((uint32_t)air_index() << 28) | (++g_cmd_seq & 0x0FFFFFFF);
        air_out(g_txbuf, 12 + len, AIR_FRAME, g_cmd_id, pre);
        break;
    case 5:                                                       /* MP reply */
        inc_tx_count(s);
        air_out(g_txbuf, 12 + len, AIR_REPLY, g_rx_cmd_id, pre);
        break;
    case 4: {                                                     /* beacon: the timestamp when it goes out */
        uint64_t ts = g_us_counter + (uint64_t)pre;
        memcpy(&g_txbuf[0xC + 24], &ts, 8);
        air_out(g_txbuf, 12 + len, AIR_FRAME, 0, pre);
        break;
    }
    }
}

static void start_tx_loc(int n, int loc) {
    TXSlot* s = &g_tx[n];
    s->valid = 1; s->sent = 0;
    s->addr = (uint16_t)((IOPORT(W_TXSlotLoc1 + loc * 4) & 0x0FFF) << 1);
    s->length = RAM16(s->addr + 0xA) & 0x3FFF;
    s->rate = g_ram[s->addr + 8] == 0x14 ? 2 : 1;
    s->phase = 0; s->phase_time = preamble_len(s->rate);
}
static void start_tx_cmd(void) {
    TXSlot* s = &g_tx[1];
    s->valid = 1; s->sent = 0;
    s->addr = (uint16_t)((IOPORT(W_TXSlotCmd) & 0x0FFF) << 1);
    s->length = RAM16(s->addr + 0xA) & 0x3FFF;
    s->rate = g_ram[s->addr + 8] == 0x14 ? 2 : 1;
    g_mp_client_mask = RAM16(s->addr + 12 + 24 + 2) & g_mp_client_fail;
    g_mp_client_fail &= g_mp_client_mask;
    uint32_t dur = (uint32_t)preamble_len(s->rate) + s->length * (s->rate == 2 ? 4u : 8u);
    dur += 112 + (10u + IOPORT(W_CmdReplyTime)) * (uint32_t)num_clients(g_mp_client_mask);
    dur += 32 * (s->rate == 2 ? 4u : 8u);
    if (g_cmd_counter > dur + 100) { s->phase = 0; s->phase_time = preamble_len(s->rate); }
    else { s->phase = 13; s->phase_time = g_cmd_counter > 100 ? (int)g_cmd_counter - 100 : TICK; }
    update_power_status(1);                                       /* a CMD wakes the transceiver up */
}
static void start_tx_beacon(void) {
    TXSlot* s = &g_tx[4];
    s->valid = 1; s->sent = 0;
    s->addr = (uint16_t)((IOPORT(W_TXSlotBeacon) & 0x0FFF) << 1);
    s->length = RAM16(s->addr + 0xA) & 0x3FFF;
    s->rate = g_ram[s->addr + 8] == 0x14 ? 2 : 1;
    s->phase = 0; s->phase_time = preamble_len(s->rate);
    IOPORT(W_TXBusy) |= 0x0010;
}

static void fire_tx(void) {
    if (!(IOPORT(W_RXCnt) & 0x8000)) return;
    uint16_t busy = IOPORT(W_TXBusy), req = IOPORT(W_TXReqRead), start = 0;
    if (IOPORT(W_TXSlotLoc1) & 0x8000) start |= 1;
    if (IOPORT(W_TXSlotCmd) & 0x8000) start |= 2;
    if (IOPORT(W_TXSlotLoc2) & 0x8000) start |= 4;
    if (IOPORT(W_TXSlotLoc3) & 0x8000) start |= 8;
    start &= req; start &= (uint16_t)~busy;
    IOPORT(W_TXBusy) = busy | start;
    if (start & 8) { start_tx_loc(3, 2); return; }
    if (start & 4) { start_tx_loc(2, 1); return; }
    if (start & 2) { g_mp_client_fail = 0xFFFE; start_tx_cmd(); return; }
    if (start & 1) { start_tx_loc(0, 0); return; }
}

/* An empty MP reply, when the client has nothing ready. */
static void send_mp_default_reply(void) {
    uint8_t r[12 + 28];
    memset(r, 0, sizeof r);
    put16(r + 0xA, 28);
    r[8] = 0x14;
    if (g_channel == 0) return;
    r[9] = (uint8_t)g_channel;
    uint8_t* p = r + 0xC;
    p = put16(p, 0x0158); p = put16(p, 0x00F0);
    p = put16(p, IOPORT(W_BSSID0)); p = put16(p, IOPORT(W_BSSID1)); p = put16(p, IOPORT(W_BSSID2));
    p = put16(p, IOPORT(W_MACAddr0)); p = put16(p, IOPORT(W_MACAddr1)); p = put16(p, IOPORT(W_MACAddr2));
    p = put16(p, 0x0903); p = put16(p, 0x00BF); p = put16(p, 0x1000);
    p = put16(p, (uint16_t)(IOPORT(W_TXSeqNo) << 4)); put32(p, 0);
    air_out(r, sizeof r, AIR_REPLY, g_rx_cmd_id, 96);
}

/* A client received a CMD addressed to it: the prepared reply goes out. */
static void send_mp_reply(uint16_t clienttime, uint16_t clientmask) {
    TXSlot* s = &g_tx[5];
    if (IOPORT(W_TXSlotReply2) & 0x8000) RAM16(s->addr) = 0x0001;
    s->rate = 2;
    IOPORT(W_TXSlotReply2) = IOPORT(W_TXSlotReply1);
    IOPORT(W_TXSlotReply1) = 0;
    if (!(IOPORT(W_TXSlotReply2) & 0x8000)) s->valid = 0;
    else {
        s->valid = 1;
        s->addr = (uint16_t)((IOPORT(W_TXSlotReply2) & 0x0FFF) << 1);
        s->length = RAM16(s->addr + 0xA) & 0x3FFF;
        uint32_t dur = (uint32_t)preamble_len(s->rate) + s->length * 4u;
        if (dur > clienttime) s->valid = 0;                       /* too long for the reply window: not sent */
    }
    if (s->valid) { s->phase = 0; s->sent = 1; tx_send_frame(s, 5); }
    else { s->phase = 10; s->sent = 1; send_mp_default_reply(); }
    int clientnum = 0;
    for (int i = 1; i < IOPORT(W_AIDLow); i++) if (clientmask & (1 << i)) clientnum++;
    s->phase_time = 16 + (clienttime + 10) * clientnum + preamble_len(s->rate);
    IOPORT(W_TXBusy) |= 0x0080;
}

/* The host's acknowledgement after the reply window. */
static void send_mp_ack(uint16_t cmdcount, uint16_t clientfail) {
    uint8_t a[12 + 32];
    memset(a, 0, sizeof a);
    put16(a + 0xA, 32);
    a[8] = g_tx[1].rate == 2 ? 0x14 : 0x0A;
    if (g_channel == 0) return;
    a[9] = (uint8_t)g_channel;
    uint8_t* p = a + 0xC;
    p = put16(p, 0x0218); p = put16(p, 0);
    p = put16(p, 0x0903); p = put16(p, 0x00BF); p = put16(p, 0x0300);
    p = put16(p, IOPORT(W_BSSID0)); p = put16(p, IOPORT(W_BSSID1)); p = put16(p, IOPORT(W_BSSID2));
    p = put16(p, IOPORT(W_MACAddr0)); p = put16(p, IOPORT(W_MACAddr1)); p = put16(p, IOPORT(W_MACAddr2));
    p = put16(p, (uint16_t)(IOPORT(W_TXSeqNo) << 4));
    p = put16(p, cmdcount); p = put16(p, clientfail); put32(p, 0);
    air_out(a, sizeof a, AIR_FRAME, 0, preamble_len(g_tx[1].rate));
}

static void start_rx(void);
static void client_reply_rx(const AirFrame* f);

static int process_tx(TXSlot* s, int num) {
    if (s->phase == 0 && !s->sent && num != 5) { s->sent = 1; tx_send_frame(s, num); }   /* on the air from the start */
    s->phase_time -= TICK;
    if (s->phase_time > 0) {
        if (s->phase == 1 && !(s->phase_time & s->halfword_mask)) IOPORT(W_RXTXAddr)++;
        else if (s->phase == 2) {                                 /* MP host: the reply window, one slot per client */
            g_mp_reply_timer -= TICK;
            if (g_mp_reply_timer <= 0 && g_mp_client_mask) {
                int n = 1;
                while (!(g_mp_client_mask & (1 << n))) n++;
                AirFrame f;
                if (air_take_reply(g_tick, g_cmd_id, n, &f)) { g_mp_client_fail &= (uint16_t)~(1 << n); client_reply_rx(&f); }
                g_mp_reply_timer += 10 + IOPORT(W_CmdReplyTime);
                g_mp_client_mask &= (uint16_t)~(1 << n);
            }
        }
        return 0;
    }
    switch (s->phase) {
    case 0: {                                                     /* preamble done */
        set_irq(7);
        set_status(num == 5 ? 8 : 3);
        uint32_t len = s->length;
        if (s->rate == 2) { len *= 4; s->halfword_mask = 0x7 & TIME_CHECK_MASK; }
        else { len *= 8; s->halfword_mask = 0xF & TIME_CHECK_MASK; }
        s->phase = 1; s->phase_time = (int)len;
        IOPORT(W_RXTXAddr) = s->addr >> 1;
        return 0;
    }
    case 10:                                                      /* preamble done (empty MP reply) */
        set_irq(7); set_status(8);
        s->phase = 11; s->phase_time = 28 * 4; s->halfword_mask = 0xFFFFFFFF;
        return 0;
    case 1:                                                       /* transmit done */
        if (num != 1 && num != 5) RAM16(s->addr) = 0x0001;
        g_ram[s->addr + 5] = 0;
        if (num == 1) {                                           /* MP CMD sent: now the replies */
            if (IOPORT(W_TXStatCnt) & 0x4000) { IOPORT(W_TXStat) = 0x0800; set_irq(1); }
            set_status(5);
            g_mp_reply_timer = 16 + preamble_len(s->rate);
            s->phase = 2;
            s->phase_time = 112 + (10 + IOPORT(W_CmdReplyTime)) * num_clients(g_mp_client_mask);
            return 0;
        }
        if (num == 5) {
            if (IOPORT(W_TXStatCnt) & 0x1000) { IOPORT(W_TXStat) = 0x0401; set_irq(1); }
            set_status(1);
            IOPORT(W_TXBusy) &= (uint16_t)~0x80;
            fire_tx();
            return 1;
        }
        IOPORT(W_TXBusy) &= (uint16_t)~(1 << num);
        if (num == 0 || num == 2 || num == 3) {
            int k = num ? num - 1 : 0;
            IOPORT(W_TXStat) = (uint16_t)(0x0001 | (k << 12));
            set_irq(1);
            IOPORT(W_TXSlotLoc1 + k * 4) &= 0x7FFF;
        } else if (num == 4 && (IOPORT(W_TXStatCnt) & 0x8000)) {
            IOPORT(W_TXStat) = 0x0301; set_irq(1);
        }
        set_status(1);
        fire_tx();
        return 1;
    case 11:                                                      /* empty MP reply done */
        IOPORT(W_TXSeqNo) = (IOPORT(W_TXSeqNo) + 1) & 0x0FFF;
        IOPORT(W_TXBusy) &= (uint16_t)~0x80;
        set_status(1);
        fire_tx();
        return 1;
    case 2: {                                                     /* MP host: reply window over, send the ACK */
        set_irq(7);
        set_status(8);
        IOPORT(W_RXTXAddr) = 0xFC0;
        s->phase_time = 32 * (s->rate == 2 ? 4 : 8);
        for (int i = 1; i < 16; i++) if (g_mp_client_fail & (1 << i)) IOPORT8(W_CMDStat0 + i)++;
        send_mp_ack((uint16_t)((g_cmd_counter + 9) / 10), g_mp_client_fail);
        s->phase = 3;
        return 0;
    }
    case 3:                                                       /* MP host: ACK sent, CMD done */
        RAM16(s->addr) = g_mp_client_fail ? 0x0005 : 0x0001;
        RAM16(s->addr + 2) = g_mp_client_fail;                    /* which clients did not answer */
        if (!g_mp_client_fail) inc_tx_count(s);
        IOPORT(W_TXSeqNo) = (IOPORT(W_TXSeqNo) + 1) & 0x0FFF;
        if (IOPORT(W_TXStatCnt) & 0x2000) { IOPORT(W_TXStat) = 0x0B01; set_irq(1); }
        IOPORT(W_TXBusy) &= (uint16_t)~2;
        IOPORT(W_TXSlotCmd) &= 0x7FFF;
        set_status(1);
        set_irq(12);
        fire_tx();
        return 1;
    case 13:                                                      /* MP CMD: no time left in CMDCOUNT */
        IOPORT(W_TXBusy) &= (uint16_t)~2;
        IOPORT(W_TXSlotCmd) &= 0x7FFF;
        RAM16(s->addr) = 0x0005;
        IOPORT(W_TXSeqNo) = (IOPORT(W_TXSeqNo) + 1) & 0x0FFF;
        set_status(1);
        set_irq(12);
        fire_tx();
        return 1;
    }
    return 0;
}

static void inc_rx_addr(uint16_t* a, int inc) {
    for (int i = 0; i < inc; i += 2) {
        *a = (uint16_t)((*a + 2) & 0x1FFE);
        if (*a == (IOPORT(W_RXBufEnd) & 0x1FFE)) *a = IOPORT(W_RXBufBegin) & 0x1FFE;
    }
}

static void start_rx(void) {
    uint16_t len = (uint16_t)(g_rxbuf[8] | g_rxbuf[9] << 8), rate = (uint16_t)(g_rxbuf[6] | g_rxbuf[7] << 8);
    g_rx_time = len;
    if (rate == 0x14) { g_rx_time *= 4; g_rx_halfword_mask = 0x7 & TIME_CHECK_MASK; }
    else { g_rx_time *= 8; g_rx_halfword_mask = 0xF & TIME_CHECK_MASK; }
    uint16_t a = (uint16_t)(IOPORT(W_RXBufWriteCursor) << 1);
    inc_rx_addr(&a, 12);
    IOPORT(W_RXTXAddr) = a >> 1;
    g_rxbuf_ptr = 12;
    set_irq(6);
    set_status(6);
    g_com |= 1;
}

static void finish_rx(void) {
    g_com &= ~1u;
    g_rx_counter = 0;
    if (!g_com) {
        if (IOPORT(W_PowerState) & (1 << 9)) { IOPORT(W_TRXPower) = 0; set_status(9); }
        else set_status(1);
    }
    uint16_t fc = (uint16_t)(g_rxbuf[12] | g_rxbuf[13] << 8);
    uint16_t seqno = (uint16_t)(g_rxbuf[12 + 22] | g_rxbuf[12 + 23] << 8);
    const uint8_t* dst = &g_rxbuf[12 + 4];
    if (!(dst[0] & 1) && !mac_eq(dst, (const uint8_t*)&IOPORT(W_MACAddr0))) return;
    if ((fc & (1 << 14)) && !(IOPORT(W_WEPCnt) & (1 << 15))) return;
    uint16_t flags = 0x0010;
    int cmd_dupe = 0;
    const uint8_t* bssid_reg = (const uint8_t*)&IOPORT(W_BSSID0);
    switch ((fc >> 2) & 3) {
    case 0: {                                                     /* management */
        if (mac_eq(&g_rxbuf[12 + 16], bssid_reg)) flags |= 0x8000;
        int sub = (fc >> 4) & 0xF;
        if (sub == 0x8) { if (!(flags & 0x8000) && !(IOPORT(W_RXFilter) & 1)) return; flags |= 1; }
        else if (sub <= 5 || (sub >= 0xA && sub <= 0xC)) { if (!(flags & 0x8000) && !(IOPORT(W_RXFilter) & (3 << 9))) return; }
        break;
    }
    case 1:                                                       /* control: only PS-poll */
        if ((fc & 0xF0) != 0xA0) return;
        if (mac_eq(&g_rxbuf[12 + 4], bssid_reg)) flags |= 0x8000;
        if (!(flags & 0x8000) && !(IOPORT(W_RXFilter) & (1 << 11))) return;
        flags |= 0x0005;
        break;
    case 2: {                                                     /* data */
        int fromto = (fc >> 8) & 3;
        if (IOPORT(W_RXFilter2) & (1 << fromto)) return;
        static const int boff[4] = { 16, 4, 10, 0 };
        if (boff[fromto] && mac_eq(&g_rxbuf[12 + boff[fromto]], bssid_reg)) flags |= 0x8000;
        uint16_t filt = IOPORT(W_RXFilter);
        if (!(flags & 0x8000) && !(filt & (1 << 11))) return;
        if ((fc & (1 << 11)) && !(filt & 1)) return;
        /* MP frames: the hardware looks at these addresses */
        if (mac_eq(&g_rxbuf[12 + 16], MP_REPLY_MAC)) flags |= (fc & 0xF0) == 0x50 ? 0x000F : 0x000E;
        else if (mac_eq(&g_rxbuf[12 + 4], MP_CMD_MAC)) { if (seqno == g_mp_last_seqno) cmd_dupe = 1; g_mp_last_seqno = seqno; flags |= 0x000C; }
        else if (mac_eq(&g_rxbuf[12 + 4], MP_ACK_MAC)) flags |= 0x000D;
        else flags |= 0x0008;
        switch ((fc >> 4) & 0xF) {
        case 0x0: case 0x4: break;
        case 0x1:
            if ((flags & 0xF) == 0xD) { if (!(filt & (1 << 7))) return; }
            else if ((flags & 0xF) != 0xE) { if (!(filt & (1 << 1))) return; }
            break;
        case 0x2: if ((flags & 0xF) != 0xC && !(filt & (1 << 2))) return; break;
        case 0x3: if (!(filt & (1 << 3))) return; break;
        case 0x5:
            if ((flags & 0xF) == 0xF) { if (!(filt & (1 << 8))) return; }
            else if (!(filt & (1 << 4))) return;
            break;
        case 0x6: if (!(filt & (1 << 5))) return; break;
        case 0x7: if (!(filt & (1 << 6))) return; break;
        default: return;
        }
        break;
    }
    }
    if (!cmd_dupe) {
        uint16_t h = (uint16_t)(IOPORT(W_RXBufWriteCursor) << 1);
        RAM16(h) = flags; inc_rx_addr(&h, 2);
        RAM16(h) = 0x0040; inc_rx_addr(&h, 4);
        RAM16(h) = (uint16_t)(g_rxbuf[6] | g_rxbuf[7] << 8); inc_rx_addr(&h, 2);
        RAM16(h) = (uint16_t)(g_rxbuf[8] | g_rxbuf[9] << 8); inc_rx_addr(&h, 2);
        RAM16(h) = 0x4080;                                        /* RSSI */
        uint16_t a = (uint16_t)(IOPORT(W_RXTXAddr) << 1);
        if (a & 2) inc_rx_addr(&a, 2);
        IOPORT(W_RXBufWriteCursor) = (uint16_t)((a & ~3u) >> 1);
        set_irq(0);
    }
    if ((flags & 0x800F) == 0x800C) {                             /* a CMD from our host: reply if asked to */
        uint16_t clienttime = (uint16_t)(g_rxbuf[0xC + 24] | g_rxbuf[0xC + 25] << 8);
        uint16_t clientmask = (uint16_t)(g_rxbuf[0xC + 26] | g_rxbuf[0xC + 27] << 8);
        if (IOPORT(W_AIDLow) && (clientmask & (1 << IOPORT(W_AIDLow)))) send_mp_reply(clienttime, clientmask);
    } else if ((flags & 0x800F) == 0x8001) {                      /* beacon from our BSS: take its timestamp */
        uint32_t len = (uint32_t)(g_rxbuf[8] | g_rxbuf[9] << 8);
        len *= (g_rxbuf[6] == 0x14) ? 4 : 8;
        len -= 76;
        uint64_t ts; memcpy(&ts, &g_rxbuf[12 + 24], 8);
        g_us_counter = ts + len;
    }
}

/* crop a received frame the way RXLENCROP says; the length goes in [8], the rate in [6] */
static int crop_rx(int flen) {
    uint16_t fc = (uint16_t)(g_rxbuf[12] | g_rxbuf[13] << 8);
    uint8_t rate = g_rxbuf[8];
    if (fc & (1 << 14)) {
        flen -= (IOPORT(W_RXLenCrop) >> 7) & 0x1FE;
        if (flen > 24) memmove(&g_rxbuf[12 + 24], &g_rxbuf[12 + 28], (size_t)flen);
    } else flen -= (IOPORT(W_RXLenCrop) << 1) & 0x1FE;
    if (flen < 0) flen = 0;
    g_rxbuf[6] = rate; g_rxbuf[7] = 0;
    g_rxbuf[8] = (uint8_t)flen; g_rxbuf[9] = (uint8_t)(flen >> 8);
    return flen;
}

static int rx_ready(void) {
    if (IOPORT(W_PowerState) & (1 << 9)) return 0;
    if (!(IOPORT(W_RXCnt) & 0x8000)) return 0;
    if (IOPORT(W_RXBufBegin) == IOPORT(W_RXBufEnd)) return 0;
    return 1;
}

/* MP host: a client's reply, received in its slot of the reply window */
static void client_reply_rx(const AirFrame* f) {
    if (!rx_ready()) return;
    if (f->len < 12 || f->len > (int)sizeof g_rxbuf) return;
    memcpy(g_rxbuf, f->data, f->len);
    int flen = g_rxbuf[10] | g_rxbuf[11] << 8;
    if (flen > f->len - 12) flen = f->len - 12;
    crop_rx(flen);
    g_rx_cmd_id = 0;
    start_rx();
}

/* frames from other consoles: every tick; the access point: every 512 us */
static int check_rx(int ap_too) {
    if (!rx_ready()) return 0;
    AirFrame f;
    while (air_active() && air_recv(g_tick, &f)) {
        if (f.len < 12 + 24 || f.len > (int)sizeof g_rxbuf) continue;
        memcpy(g_rxbuf, f.data, f.len);
        int flen = g_rxbuf[10] | g_rxbuf[11] << 8;
        if (flen != f.len - 12) continue;
        if (g_rxbuf[9] != g_channel || g_channel == 0) continue;
        crop_rx(flen);
        g_rx_cmd_id = f.cmd_id;
        start_rx();
        return 1;
    }
    if (!ap_too) return 0;
    for (;;) {
        int n = ap_recv(g_rxbuf);
        if (n <= 0) return 0;
        if (n < 12 + 24) continue;
        int flen = g_rxbuf[10] | g_rxbuf[11] << 8;
        if (flen != n - 12) continue;
        if (g_rxbuf[9] != g_channel || g_channel == 0) continue;
        crop_rx(flen);
        g_rx_cmd_id = 0;
        start_rx();
        return 1;
    }
}

static void ms_timer(void) {
    if (IOPORT(W_USCompareCnt) && (g_us_counter & ~0x3FFull) == g_us_compare) { g_block_beacon_irq14 = 0; set_irq14(0); }
    if (IOPORT(W_BeaconCount1)) { if (--IOPORT(W_BeaconCount1) == 0) set_irq14(1); }
    if (IOPORT(W_BeaconCount1) == 0) IOPORT(W_BeaconCount1) = IOPORT(W_BeaconInterval);
    if (IOPORT(W_BeaconCount2)) { if (--IOPORT(W_BeaconCount2) == 0) set_irq13(); }
}

static void us_tick(void) {
    g_us_timestamp += TICK;
    if (!(g_us_timestamp & 0x3FF & TIME_CHECK_MASK)) ap_ms_timer();
    if (g_us_until_power_on < 0) {
        g_us_until_power_on += TICK;
        if (g_us_until_power_on >= 0) { g_us_until_power_on = 0; IOPORT(W_PowerState) = 0; set_status(1); update_power_status(0); }
    }
    if (IOPORT(W_USCountCnt)) {
        g_us_counter += TICK;
        uint32_t uspart = (uint32_t)(g_us_counter & 0x3FF);
        if (IOPORT(W_USCompareCnt)) {
            uint32_t beaconus = ((uint32_t)IOPORT(W_BeaconCount1) << 10) | (0x3FF - uspart);
            if ((beaconus & TIME_CHECK_MASK) == (IOPORT(W_PreBeacon) & TIME_CHECK_MASK)) set_irq15();
        }
        if (!(uspart & TIME_CHECK_MASK)) ms_timer();
    }
    if ((IOPORT(W_CmdCountCnt) & 1) && g_cmd_counter) g_cmd_counter = g_cmd_counter < TICK ? 0 : g_cmd_counter - TICK;
    if (IOPORT(W_ContentFree)) IOPORT(W_ContentFree) = IOPORT(W_ContentFree) < TICK ? 0 : IOPORT(W_ContentFree) - TICK;
    if (g_com == 0) {
        uint16_t busy = IOPORT(W_TXBusy);
        if (busy) {
            if (IOPORT(W_PowerState) & (1 << 9)) { g_com = 0; g_tx_cur = -1; }
            else {
                g_com = 2;
                g_tx_cur = (busy & 0x80) ? 5 : (busy & 0x10) ? 4 : (busy & 8) ? 3 : (busy & 4) ? 2 : (busy & 2) ? 1 : 0;
            }
        } else {
            check_rx(!(g_rx_counter & 0x1FF & TIME_CHECK_MASK));
            g_rx_counter += TICK;
        }
    }
    if (g_com & 2) {
        if (process_tx(&g_tx[g_tx_cur], g_tx_cur)) {
            if (IOPORT(W_PowerState) & (1 << 9)) { IOPORT(W_TXBusy) = 0; IOPORT(W_TRXPower) = 0; set_status(9); }
            uint16_t busy = IOPORT(W_TXBusy);
            if (busy & 0x80) g_tx_cur = 5;
            else if (busy & 0x10) g_tx_cur = 4; else if (busy & 8) g_tx_cur = 3; else if (busy & 4) g_tx_cur = 2;
            else if (busy & 2) g_tx_cur = 1; else if (busy & 1) g_tx_cur = 0;
            else { g_tx_cur = -1; g_com = 0; g_rx_counter = 0; }
        }
    }
    if (g_com & 1) {
        g_rx_time -= TICK;
        if (!(g_rx_time & g_rx_halfword_mask)) {
            uint16_t a = (uint16_t)(IOPORT(W_RXTXAddr) << 1);
            if (a < 0x1FFF) memcpy(&g_ram[a], &g_rxbuf[g_rxbuf_ptr & 0x7FE], 2);
            inc_rx_addr(&a, 2);
            IOPORT(W_RXTXAddr) = a >> 1;
            g_rxbuf_ptr += 2;
            if (g_rx_time <= 0) finish_rx();
            else if (a == (uint16_t)(IOPORT(W_RXBufReadCursor) << 1)) {      /* RX buffer full: drop the frame */
                g_rx_time = 0;
                set_status(1);
                if (g_tx_cur < 0) { g_com &= ~1u; g_rx_counter = 0; }
                if (!g_com && (IOPORT(W_PowerState) & (1 << 9))) { IOPORT(W_TRXPower) = 0; set_status(9); }
            }
        }
    }
}

/* Called as emulated time passes (bus cycles at 33.51 MHz). */
void wifi_advance(uint32_t cycles) {
    g_cycle_acc += cycles * 1000u;                              /* in 1/1000 cycles: 8 us = 268112 */
    if (!air_active()) {
        while (g_cycle_acc >= 268112u) { g_cycle_acc -= 268112u; g_tick++; if (g_power_on) us_tick(); }
        return;
    }
    while (g_cycle_acc >= 268112u) {
        g_cycle_acc -= 268112u;
        air_reach(g_tick, g_power_on);                          /* listening: the others must have caught up */
        if (g_power_on) us_tick(); else air_skip(g_tick);
        g_tick++;
    }
    air_reach(g_tick, 0);
}

static void change_channel(void) {
    uint32_t v1 = g_rf[g_rf_chan_index[0] & 0x3F], v2 = g_rf[g_rf_chan_index[1] & 0x3F];
    g_channel = 0;
    for (int i = 0; i < 14; i++) if (v1 == g_rf_chan_data[i][0] && v2 == g_rf_chan_data[i][1]) { g_channel = i + 1; break; }
    WLOG("wifi: channel %d\n", g_channel);
}
static void rf_transfer_type3(void) {
    uint32_t id = (IOPORT(W_RFData1) >> 8) & 0x3F, cmd = IOPORT(W_RFData2) & 0xF;
    if (cmd == 6) IOPORT(W_RFData1) = (uint16_t)((IOPORT(W_RFData1) & 0xFF00) | (g_rf[id] & 0xFF));
    else if (cmd == 5) {
        g_rf[id] = IOPORT(W_RFData1) & 0xFF;
        WLOG("wifi: RF[%02x] = %02x\n", id, g_rf[id]);
        if (id == g_rf_chan_index[0] || id == g_rf_chan_index[1]) change_channel();
    }
}
static void rf_transfer_type2(void) {
    uint32_t id = (IOPORT(W_RFData2) >> 2) & 0x1F;
    if (IOPORT(W_RFData2) & 0x80) {
        IOPORT(W_RFData1) = (uint16_t)g_rf[id];
        IOPORT(W_RFData2) = (uint16_t)((IOPORT(W_RFData2) & 0xFFFC) | ((g_rf[id] >> 16) & 3));
    } else {
        g_rf[id] = IOPORT(W_RFData1) | ((uint32_t)(IOPORT(W_RFData2) & 3) << 16);
        if (id == g_rf_chan_index[0] || id == g_rf_chan_index[1]) change_channel();
    }
}

static void update_power_on(void) {
    int on = g_enabled && !(IOPORT(W_PowerUS) & 1);
    if (on == g_power_on) return;
    g_power_on = on;
    WLOG("wifi: %s\n", on ? "on" : "off");
}

/* POWCNT2 bit 1 */
void wifi_set_power(int on) { g_enabled = on; update_power_on(); }

void wifi_reset(const uint8_t* fw) {
    memset(g_ram, 0, sizeof g_ram); memset(w_io, 0, sizeof w_io);
    g_enabled = g_power_on = 0;
    g_random = 1;
    memset(g_bb, 0, sizeof g_bb); memset(g_bb_ro, 0, sizeof g_bb_ro);
    static const struct { uint8_t id, v; } fixed[] = { { 0x00, 0x6D }, { 0x0D, 0 }, { 0x0E, 0 }, { 0x0F, 0 }, { 0x10, 0 }, { 0x11, 0 },
        { 0x12, 0 }, { 0x16, 0 }, { 0x17, 0 }, { 0x18, 0 }, { 0x19, 0 }, { 0x1A, 0 }, { 0x27, 0 }, { 0x4D, 0 }, { 0x5D, 1 },
        { 0x5E, 0 }, { 0x5F, 0 }, { 0x60, 0 }, { 0x61, 0 }, { 0x64, 0xFF }, { 0x66, 0 } };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) { g_bb[fixed[i].id] = fixed[i].v; g_bb_ro[fixed[i].id] = 1; }
    for (int i = 0x69; i < 0x100; i++) g_bb_ro[i] = 1;
    /* RF chip type and the channel tables from the firmware header (type 3 layout) */
    g_rf_version = fw[0x40];
    memset(g_rf, 0, sizeof g_rf);
    if (g_rf_version == 3) {
        g_rf_chan_index[0] = fw[0x116]; g_rf_chan_index[1] = fw[0x125];
        for (int i = 0; i < 14; i++) { g_rf_chan_data[i][0] = fw[0x117 + i]; g_rf_chan_data[i][1] = fw[0x126 + i]; }
    } else {
        const uint8_t* v = &fw[0xCE + 36];                         /* type 2: InitialRF56Values */
        g_rf_chan_index[0] = v[2] >> 2; g_rf_chan_index[1] = v[5] >> 2;
        for (int i = 0; i < 14; i++) {
            g_rf_chan_data[i][0] = v[i * 6] | v[i * 6 + 1] << 8 | (uint32_t)(v[i * 6 + 2] & 3) << 16;
            g_rf_chan_data[i][1] = v[i * 6 + 3] | v[i * 6 + 4] << 8 | (uint32_t)(v[i * 6 + 5] & 3) << 16;
        }
    }
    g_channel = 0;
    IOPORT(W_ID) = fw[0x1D] == 0xFF ? 0x1440 : 0xC340;             /* original DS / DS Lite chip */
    memset(&IOPORT(W_MACAddr0), 0xFF, 6);
    memset(&IOPORT(W_BSSID0), 0xFF, 6);
    IOPORT(W_PowerUS) = 1;
    g_us_timestamp = g_us_counter = g_us_compare = 0;
    g_block_beacon_irq14 = 0;
    memset(g_tx, 0, sizeof g_tx);
    g_com = 0; g_tx_cur = -1; g_rx_counter = 0;
    g_rxbuf_ptr = 0; g_rx_time = 0; g_rx_halfword_mask = 0xFFFFFFFF;
    g_cmd_counter = 0; g_us_until_power_on = 0;
    g_tick = 0; g_cycle_acc = 0; g_mp_reply_timer = 0; g_mp_client_mask = g_mp_client_fail = 0; g_mp_last_seqno = 0xFFFF;
    g_cmd_seq = g_cmd_id = g_rx_cmd_id = 0;
    ap_reset();
    { void air_init_from_env(void); air_init_from_env(); }
}

uint16_t wifi_read16(uint32_t addr) {
    if (addr >= 0x04810000) return 0;
    addr &= 0x7FFE;
    if (addr >= 0x4000 && addr < 0x6000) return RAM16(addr & 0x1FFE);
    if (addr >= 0x2000 && addr < 0x4000) return 0xFFFF;
    int active = addr < 0x1000;
    switch (addr) {
    case W_Random: g_random = (uint16_t)((g_random & 1) ^ (((g_random & 0x3FF) << 1) | (g_random >> 10))); return g_random;
    case W_Preamble: return IOPORT(W_Preamble) & 3;
    case W_USCount0: return (uint16_t)g_us_counter;
    case W_USCount1: return (uint16_t)(g_us_counter >> 16);
    case W_USCount2: return (uint16_t)(g_us_counter >> 32);
    case W_USCount3: return (uint16_t)(g_us_counter >> 48);
    case W_USCompare0: return (uint16_t)g_us_compare;
    case W_USCompare1: return (uint16_t)(g_us_compare >> 16);
    case W_USCompare2: return (uint16_t)(g_us_compare >> 32);
    case W_USCompare3: return (uint16_t)(g_us_compare >> 48);
    case W_CmdCount: return (uint16_t)((g_cmd_counter + 9) / 10);
    case W_BBRead: return ((IOPORT(W_BBCnt) & 0xF000) == 0x6000) ? g_bb[IOPORT(W_BBCnt) & 0xFF] : 0;
    case W_BBBusy: case W_RFBusy: return 0;
    case W_RXBufDataRead:
        if (active) {
            uint32_t rd = IOPORT(W_RXBufReadAddr);
            uint16_t ret = RAM16(rd & 0x1FFE);
            rd += 2;
            if (rd == (IOPORT(W_RXBufEnd) & 0x1FFEu)) rd = IOPORT(W_RXBufBegin) & 0x1FFE;
            if (rd == IOPORT(W_RXBufGapAddr)) {
                rd += (uint32_t)IOPORT(W_RXBufGapSize) << 1;
                if (rd >= (IOPORT(W_RXBufEnd) & 0x1FFEu)) rd = rd + (IOPORT(W_RXBufBegin) & 0x1FFE) - (IOPORT(W_RXBufEnd) & 0x1FFE);
                if (IOPORT(W_ID) == 0xC340) IOPORT(W_RXBufGapSize) = 0;
            }
            IOPORT(W_RXBufReadAddr) = (uint16_t)(rd & 0x1FFE);
            IOPORT(W_RXBufDataRead) = ret;
            if (IOPORT(W_RXBufCount) > 0 && --IOPORT(W_RXBufCount) == 0) set_irq(9);
        }
        break;
    case W_TXBusy: return IOPORT(W_TXBusy) & 0x1F;
    default:
        if (addr >= W_CMDStat0 && addr <= W_CMDStat0 + 0xE) { uint16_t r = IOPORT(addr & 0xFFF); IOPORT(addr & 0xFFF) = 0; return r; }
        break;
    }
    return IOPORT(addr & 0xFFF);
}

void wifi_write16(uint32_t addr, uint16_t val) {
    if (addr >= 0x04810000) return;
    addr &= 0x7FFE;
    if (addr >= 0x4000 && addr < 0x6000) { RAM16(addr & 0x1FFE) = val; return; }
    if (addr >= 0x2000 && addr < 0x4000) return;
    switch (addr) {
    case W_ModeReset: {
        uint16_t old = IOPORT(W_ModeReset);
        IOPORT(W_ModeReset) = val & 1;
        if (!(old & 1) && (val & 1)) { IOPORT(0x27C) = 5; update_power_status(0); }
        else if ((old & 1) && !(val & 1)) { IOPORT(0x27C) = 0xA; update_power_status(0); }
        if (val & 0x2000) { IOPORT(W_RXBufWriteAddr) = 0; IOPORT(W_CmdTotalTime) = 0; IOPORT(W_CmdReplyTime) = 0; IOPORT(0x1A4) = 0; IOPORT(0x278) = 0xF; }
        if (val & 0x4000) {
            IOPORT(W_ModeWEP) = 0; IOPORT(W_TXStatCnt) = 0; IOPORT(0x00A) = 0;
            IOPORT(W_MACAddr0) = IOPORT(W_MACAddr1) = IOPORT(W_MACAddr2) = 0;
            IOPORT(W_BSSID0) = IOPORT(W_BSSID1) = IOPORT(W_BSSID2) = 0;
            IOPORT(W_AIDLow) = 0; IOPORT(W_AIDFull) = 0; IOPORT(W_TXRetryLimit) = 0x0707; IOPORT(0x02E) = 0;
            IOPORT(W_RXBufBegin) = 0x4000; IOPORT(W_RXBufEnd) = 0x4800; IOPORT(W_TXBeaconTIM) = 0; IOPORT(W_Preamble) = 1;
            IOPORT(W_RXFilter) = 0x0401; IOPORT(0x0D4) = 1; IOPORT(W_RXFilter2) = 8; IOPORT(0x0EC) = 0x3F03;
            IOPORT(W_TXHeaderCnt) = 0; IOPORT(0x198) = 0; IOPORT(0x1A2) = 1; IOPORT(0x224) = 3; IOPORT(0x230) = 0x47;
        }
        return;
    }
    case W_ModeWEP:
        val &= 0x7F; IOPORT(W_ModeWEP) = val;
        if (IOPORT(W_PowerTX) & 2) {
            if ((val & 7) == 1) IOPORT(W_PowerDownCtrl) |= 2;
            else if ((val & 7) == 2) IOPORT(W_PowerDownCtrl) = 3;
            if ((val & 7) != 3) IOPORT(W_PowerState) &= 0x0300;
            update_power_status(0);
        }
        return;
    case W_IE: { uint16_t old = IOPORT(W_IF) & IOPORT(W_IE); IOPORT(W_IE) = val; check_irq(old); return; }
    case W_IF: IOPORT(W_IF) &= (uint16_t)~val; return;
    case W_IFSet: { uint16_t old = IOPORT(W_IF) & IOPORT(W_IE); IOPORT(W_IF) |= val & 0xFBFF; check_irq(old); return; }
    case W_AIDLow: IOPORT(W_AIDLow) = val & 0xF; return;
    case W_AIDFull: IOPORT(W_AIDFull) = val & 0x7FF; return;
    case W_PowerUS: IOPORT(W_PowerUS) = val & 3; update_power_on(); return;
    case W_PowerTX:
        IOPORT(W_PowerTX) = val & 3;
        if (val & 2) {
            if ((IOPORT(W_ModeWEP) & 7) == 1) IOPORT(W_PowerDownCtrl) |= 2;
            else if ((IOPORT(W_ModeWEP) & 7) == 2) IOPORT(W_PowerDownCtrl) = 3;
            update_power_status(0);
        }
        return;
    case W_PowerState:
        if ((IOPORT(W_ModeWEP) & 7) != 3) return;
        val = (uint16_t)((IOPORT(W_PowerState) & 0x0300) | (val & 3));
        if ((val & 0x0300) == 0x0200) val &= (uint16_t)~1; else val &= (uint16_t)~2;
        if (!(val & (1 << 9))) val &= (uint16_t)~(1 << 8);
        IOPORT(W_PowerState) = val;
        update_power_status(0);
        return;
    case W_PowerForce: IOPORT(W_PowerForce) = val & 0x8001; update_power_status(0); return;
    case W_PowerDownCtrl:
        IOPORT(W_PowerDownCtrl) = val & 3;
        if (IOPORT(W_PowerTX) & 2) {
            if ((IOPORT(W_ModeWEP) & 7) == 1) IOPORT(W_PowerDownCtrl) |= 2;
            else if ((IOPORT(W_ModeWEP) & 7) == 2) IOPORT(W_PowerDownCtrl) = 3;
        }
        update_power_status(0);
        return;
    case W_USCountCnt: val &= 1; break;
    case W_USCompareCnt: if (val & 2) set_irq14(2); val &= 1; break;
    case W_USCount0: g_us_counter = (g_us_counter & 0xFFFFFFFFFFFF0000ull) | val; return;
    case W_USCount1: g_us_counter = (g_us_counter & 0xFFFFFFFF0000FFFFull) | ((uint64_t)val << 16); return;
    case W_USCount2: g_us_counter = (g_us_counter & 0xFFFF0000FFFFFFFFull) | ((uint64_t)val << 32); return;
    case W_USCount3: g_us_counter = (g_us_counter & 0x0000FFFFFFFFFFFFull) | ((uint64_t)val << 48); return;
    case W_USCompare0:
        g_us_compare = (g_us_compare & 0xFFFFFFFFFFFF0000ull) | (val & 0xFC00);
        if (val & 1) g_block_beacon_irq14 = 1;
        return;
    case W_USCompare1: g_us_compare = (g_us_compare & 0xFFFFFFFF0000FFFFull) | ((uint64_t)val << 16); return;
    case W_USCompare2: g_us_compare = (g_us_compare & 0xFFFF0000FFFFFFFFull) | ((uint64_t)val << 32); return;
    case W_USCompare3: g_us_compare = (g_us_compare & 0x0000FFFFFFFFFFFFull) | ((uint64_t)val << 48); return;
    case W_CmdCount: g_cmd_counter = (uint32_t)val * 10; return;
    case W_BBCnt:
        IOPORT(W_BBCnt) = val;
        if ((val & 0xF000) == 0x5000 && !g_bb_ro[val & 0xFF]) g_bb[val & 0xFF] = (uint8_t)IOPORT(W_BBWrite);
        return;
    case W_RFData2: IOPORT(W_RFData2) = val; if (g_rf_version == 3) rf_transfer_type3(); else rf_transfer_type2(); return;
    case W_RFCnt: val &= 0x413F; break;
    case W_RXCnt:
        if (val & 1) IOPORT(W_RXBufWriteCursor) = IOPORT(W_RXBufWriteAddr);
        if (val & 0x80) { IOPORT(W_TXSlotReply2) = IOPORT(W_TXSlotReply1); IOPORT(W_TXSlotReply1) = 0; }
        if (val & 0x8000) fire_tx();                              /* (sees the old RXCNT, like the hardware model in melonDS) */
        val &= 0xFF0E;
        break;
    case W_RXBufDataRead:
        if (IOPORT(W_RXBufCount) > 0 && --IOPORT(W_RXBufCount) == 0) set_irq(9);
        return;
    case W_RXBufReadAddr: case W_RXBufGapAddr: val &= 0x1FFE; break;
    case W_RXBufGapSize: case W_RXBufCount: case W_RXBufWriteAddr: case W_RXBufReadCursor: val &= 0x0FFF; break;
    case W_BeaconCount2:
        /* A host sends management frames (its answers to friends joining)
         * only between its beacon and the post-beacon interrupt, about 9 ms.
         * A joining DS answers each step about a frame later than a real one
         * (the recompiled processors take turns more coarsely), so once a
         * friend was in, the next one's requests kept missing that window and
         * the join failed: 3 or 4 players could not get together. On a host
         * the window is longer. */
        if ((IOPORT(W_TXSlotBeacon) & 0x8000) && val != 0xFFFF && val < 64) val = (uint16_t)(val + POST_BEACON_EXTRA_MS);
        break;
    case W_TXReqReset: IOPORT(W_TXReqRead) &= (uint16_t)~val; return;
    case W_TXReqSet: IOPORT(W_TXReqRead) |= val; fire_tx(); return;
    case W_TXSlotReset:
        if (val & 1) IOPORT(W_TXSlotLoc1) &= 0x7FFF;
        if (val & 2) IOPORT(W_TXSlotCmd) &= 0x7FFF;
        if (val & 4) IOPORT(W_TXSlotLoc2) &= 0x7FFF;
        if (val & 8) IOPORT(W_TXSlotLoc3) &= 0x7FFF;
        if (val & 0x40) IOPORT(W_TXSlotReply2) &= 0x7FFF;
        if (val & 0x80) IOPORT(W_TXSlotReply1) &= 0x7FFF;
        val = 0;
        break;
    case W_TXBufDataWrite: {
        uint32_t wr = IOPORT(W_TXBufWriteAddr);
        RAM16(wr & 0x1FFE) = val;
        wr += 2;
        if (wr == IOPORT(W_TXBufGapAddr)) wr += (uint32_t)IOPORT(W_TXBufGapSize) << 1;
        IOPORT(W_TXBufWriteAddr) = (uint16_t)(wr & 0x1FFE);
        if (IOPORT(W_TXBufCount) > 0 && --IOPORT(W_TXBufCount) == 0) set_irq(8);
        return;
    }
    case W_TXBufWriteAddr: case W_TXBufGapAddr: val &= 0x1FFE; break;
    case W_TXBufGapSize: case W_TXBufCount: val &= 0x0FFF; break;
    case W_TXSlotCmd:
        if (g_cmd_counter == 0) val = (uint16_t)((val & 0x7FFF) | (IOPORT(W_TXSlotCmd) & 0x8000));
        /* fall through */
    case W_TXSlotLoc1: case W_TXSlotLoc2: case W_TXSlotLoc3:
        IOPORT(addr & 0xFFF) = val;
        fire_tx();
        return;
    /* read-only */
    case 0x000: case 0x034: case 0x044: case 0x054: case 0x098: case 0x0B0: case 0x0B6: case 0x0B8: case 0x15C:
    case 0x15E: case 0x180: case 0x19C: case 0x1A8: case 0x1AC: case 0x1C4: case 0x210: case 0x214: case 0x268:
        return;
    default: break;
    }
    IOPORT(addr & 0xFFF) = val;
}

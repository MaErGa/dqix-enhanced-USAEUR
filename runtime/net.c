/* The network behind the virtual access point (wifi.c).
 *
 * Off: everything the DS sends is dropped, so a connection attempt fails
 * the way it does on a DS whose access point has no internet behind it.
 *
 * On: Ethernet frames go through libslirp, a user-mode network stack (third_party/libslirp, BSD):
 * it plays the router (DHCP, ARP) and turns the DS's TCP/UDP connections
 * into ordinary sockets on this PC. The DS's own Wi-Fi settings point its
 * DNS at a community Nintendo Wi-Fi Connection server, as on real hardware
 * (see firmware_set_ap in io7.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "nds.h"

#ifdef DQIX_NO_NET
int net_send(const uint8_t* eth, int len) { (void)eth; return len; }
int net_recv(uint8_t* eth) { (void)eth; return 0; }
void net_deliver(const uint8_t* f, int len) { (void)f; (void)len; }
void net_enable(int on) { (void)on; }
void net_seal(int on) { (void)on; }
#else

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define poll WSAPoll
#else
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>
#endif
#include "libslirp.h"

static Slirp* g_slirp;
static int g_on;
/* Sealed (sessions): the DS reaches only the built-in server (wfc.c) and the
 * virtual router's address handout (DHCP, ARP), and the router's clock is the
 * game's - nothing depends on this PC's network or timing, so every PC of
 * the session computes the same. */
static int g_sealed;
void net_seal(int on) { g_sealed = on; }

/* frames from slirp to the DS */
#define QN 128
static uint8_t g_q[QN][2048];
static int g_qlen[QN], g_qhead, g_qtail;

static int g_log = -1;
static void hexlog(const char* tag, const uint8_t* p, int n) {
    if (!getenv("DQIX_NETHEX")) return;
    fprintf(stderr, "%s", tag);
    for (int i = 0; i < n; i++) fprintf(stderr, "%02x", p[i]);
    fprintf(stderr, "\n");
}
#define NLOG(...) do { if (g_log < 0) g_log = getenv("DQIX_NETLOG") != NULL; if (g_log) fprintf(stderr, __VA_ARGS__); } while (0)

/* One line per thing worth knowing (address from DHCP, each name looked up and
 * its answer, each connection opened or refused) into the bug-report log, so
 * an F10 report from a failed connection says how far it got. */
static void ev(const char* fmt, ...) {
    char line[200];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    report_log_line(line);
    NLOG("%s", line);
}
static void dns_name(const uint8_t* d, int n, int off, char* out, int cap) {
    int o = 0;
    while (off < n && d[off] && o < cap - 2) {
        int l = d[off++];
        if (l >= 0xC0) break;
        for (int i = 0; i < l && off < n && o < cap - 2; i++) out[o++] = (char)d[off++];
        if (d[off]) out[o++] = '.';
    }
    out[o] = 0;
}
static void summarize(const uint8_t* e, int len, int out) {
    if (len < 34 || e[12] != 8 || e[13] != 0) return;
    const uint8_t* ip = e + 14;
    int ihl = (ip[0] & 15) * 4, proto = ip[9];
    int n = len - 14 - ihl;
    const uint8_t* l4 = ip + ihl;
    if (n < 8) return;
    int sp = l4[0] << 8 | l4[1], dp = l4[2] << 8 | l4[3];
    const uint8_t* peer = out ? ip + 16 : ip + 12;
    if (proto == 17 && (dp == 53 || sp == 53) && n > 20) {
        const uint8_t* d = l4 + 8; int dn = n - 8;
        char name[128]; dns_name(d, dn, 12, name, sizeof name);
        if (out) { ev("net: looking up %s (DNS %u.%u.%u.%u)\n", name, peer[0], peer[1], peer[2], peer[3]); return; }
        int rcode = d[3] & 15, an = d[6] << 8 | d[7];
        /* first A record: skip the question, walk the answers */
        int o = 12; while (o < dn && d[o]) { if (d[o] >= 0xC0) { o++; break; } o += d[o] + 1; } o += 5;
        for (int i = 0; i < an && o + 12 <= dn; i++) {
            if (d[o] >= 0xC0) o += 2; else { while (o < dn && d[o]) o += d[o] + 1; o++; }
            int type = d[o] << 8 | d[o + 1], rl = d[o + 8] << 8 | d[o + 9];
            if (type == 1 && rl == 4 && o + 14 <= dn) { ev("net: %s = %u.%u.%u.%u\n", name, d[o + 10], d[o + 11], d[o + 12], d[o + 13]); return; }
            o += 10 + rl;
        }
        ev("net: %s not found (DNS answer %d)\n", name, rcode);
    } else if (proto == 17 && sp == 67 && n > 8 + 240) {
        const uint8_t* y = l4 + 8 + 16;
        ev("net: address %u.%u.%u.%u from DHCP\n", y[0], y[1], y[2], y[3]);
    } else if (proto == 6 && n >= 14) {
        int fl = l4[13];
        int port = out ? dp : sp;
        if (out && (fl & 0x12) == 0x02) ev("net: connecting to %u.%u.%u.%u:%d\n", peer[0], peer[1], peer[2], peer[3], port);
        else if (!out && (fl & 0x12) == 0x12) ev("net: connected to %u.%u.%u.%u:%d\n", peer[0], peer[1], peer[2], peer[3], port);
        else if (!out && (fl & 0x04)) ev("net: %u.%u.%u.%u:%d refused or reset the connection\n", peer[0], peer[1], peer[2], peer[3], port);
        else if (fl & 0x01) ev("net: %s closed %u.%u.%u.%u:%d\n", out ? "DS" : "server", peer[0], peer[1], peer[2], peer[3], port);
    }
}

static slirp_ssize_t cb_send(const void* buf, size_t len, void* opaque);
void net_deliver(const uint8_t* f, int len) { cb_send(f, (size_t)len, NULL); }
static slirp_ssize_t cb_send(const void* buf, size_t len, void* opaque) {
    (void)opaque;
    if (len > 2048) return (slirp_ssize_t)len;
    int next = (g_qtail + 1) % QN;
    if (next == g_qhead) { NLOG("net: queue full, frame dropped\n"); return (slirp_ssize_t)len; }
    memcpy(g_q[g_qtail], buf, len);
    g_qlen[g_qtail] = (int)len;
    g_qtail = next;
    return (slirp_ssize_t)len;
}
static void cb_error(const char* msg, void* opaque) { (void)opaque; NLOG("net: slirp error: %s\n", msg); }
static int64_t cb_clock(void* opaque) {
    (void)opaque;
    if (g_sealed) return (int64_t)g_frame * 16715000LL;          /* game time: one DS frame is about 16.715 ms */
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}
static void* cb_timer_new(SlirpTimerCb cb, void* cb_opaque, void* opaque) { (void)cb; (void)cb_opaque; (void)opaque; return NULL; }
static void cb_timer_free(void* t, void* opaque) { (void)t; (void)opaque; }
static void cb_timer_mod(void* t, int64_t when, void* opaque) { (void)t; (void)when; (void)opaque; }
static void cb_reg_fd(int fd, void* opaque) { (void)fd; (void)opaque; }
static void cb_notify(void* opaque) { (void)opaque; }
static const SlirpCb g_cb = {
    .send_packet = cb_send, .guest_error = cb_error, .clock_get_ns = cb_clock,
    .timer_new = cb_timer_new, .timer_free = cb_timer_free, .timer_mod = cb_timer_mod,
    .register_poll_fd = cb_reg_fd, .unregister_poll_fd = cb_reg_fd, .notify = cb_notify,
};

#define MAXPOLL 256
static struct pollfd g_pfd[MAXPOLL];
static int g_npfd;
static int cb_add_poll(int fd, int events, void* opaque) {
    (void)opaque;
    if (g_npfd >= MAXPOLL) return -1;
    short ev = 0;
    if (events & SLIRP_POLL_IN) ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
#ifndef _WIN32
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
#endif
    g_pfd[g_npfd].fd = fd; g_pfd[g_npfd].events = ev; g_pfd[g_npfd].revents = 0;
    return g_npfd++;
}
static int cb_get_revents(int idx, void* opaque) {
    (void)opaque;
    if (idx < 0 || idx >= g_npfd) return 0;
    short ev = g_pfd[idx].revents; int r = 0;
    if (ev & POLLIN) r |= SLIRP_POLL_IN;
    if (ev & POLLOUT) r |= SLIRP_POLL_OUT;
    if (ev & POLLPRI) r |= SLIRP_POLL_PRI;
    if (ev & POLLERR) r |= SLIRP_POLL_ERR;
    if (ev & POLLHUP) r |= SLIRP_POLL_HUP;
    return r;
}

static void start(void) {
    if (g_slirp) return;
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SlirpConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.version = 1;
    cfg.in_enabled = 1;
    inet_pton(AF_INET, "10.64.0.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.64.0.1", &cfg.vhost);
    cfg.vhostname = "dqix-pc";
    inet_pton(AF_INET, "10.64.0.16", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.64.0.2", &cfg.vnameserver);
    g_slirp = slirp_new(&cfg, &g_cb, NULL);
    ev("net: started (%s)\n", g_slirp ? "ok" : "failed");
}

static void pump(void) {
    if (!g_slirp) return;
    uint32_t timeout = 0;
    g_npfd = 0;
    slirp_pollfds_fill(g_slirp, &timeout, cb_add_poll, NULL);
    int r = g_npfd ? poll(g_pfd, (unsigned)g_npfd, 0) : 0;
    slirp_pollfds_poll(g_slirp, r < 0, cb_get_revents, NULL);
}

void net_enable(int on) { g_on = on; if (on) start(); }

int net_send(const uint8_t* eth, int len) {
    if (!g_on || !g_slirp) return len;
    if (g_log > 0) NLOG("net: DS -> %d bytes, type %02x%02x\n", len, eth[12], eth[13]);
    hexlog("TX ", eth, len);
    if (wfc_input(eth, len)) return len;                     /* the built-in server's */
    if (g_sealed) {                                           /* only ARP and DHCP go to the router */
        int arp = len >= 14 && eth[12] == 8 && eth[13] == 6;
        int dhcp = len >= 14 + 20 + 8 && eth[12] == 8 && eth[13] == 0 && eth[14 + 9] == 17 &&
                   (eth[14 + (eth[14] & 15) * 4 + 2] << 8 | eth[14 + (eth[14] & 15) * 4 + 3]) == 67;
        if (!arp && !dhcp) { summarize(eth, len, 1); return len; }
    }
    summarize(eth, len, 1);
    slirp_input(g_slirp, eth, len);
    pump();
    return len;
}

int net_recv(uint8_t* eth) {
    if (!g_on || !g_slirp) return 0;
    wfc_poll();
    if (g_qhead == g_qtail) pump();
    if (g_qhead == g_qtail) return 0;
    int n = g_qlen[g_qhead];
    memcpy(eth, g_q[g_qhead], (size_t)n);
    g_qhead = (g_qhead + 1) % QN;
    hexlog("RX ", eth, n);
    summarize(eth, n, 0);
    if (g_log > 0) NLOG("net: DS <- %d bytes, type %02x%02x\n", n, eth[12], eth[13]);
    return n;
}
#endif

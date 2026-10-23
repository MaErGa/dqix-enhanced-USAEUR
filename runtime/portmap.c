/* NAT-PMP and PCP: asking the home router to open a port when UPnP isn't
 * there (portmap.h). Both talk to the router (the default gateway) on UDP
 * port 5351. PCP is the newer one and also opens IPv6 firewalls; a router that
 * only knows NAT-PMP answers a PCP request with "unsupported version", and we
 * ask again in NAT-PMP.
 *
 * Tests: DQIX_GATEWAY=address[:port] and DQIX_GATEWAY6=address[:port] point at
 * a stand-in router instead of the real one. */
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "portmap.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
typedef SOCKET sock_t;
#define BADSOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BADSOCK (-1)
#define closesock close
#endif

#define PMP_PORT 5351
#define LIFETIME 7200                     /* seconds asked for; renewed at half of what the router grants */

static double now_s(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

static void set_timeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = { ms / 1000, (ms % 1000) * 1000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
}

static void put16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static unsigned get16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---------------------------------------------------------------- the router's address */
/* "addr" or "addr:port" / "[v6]:port" from a test variable */
static int env_addr(const char* name, int family, struct sockaddr_storage* out, socklen_t* len) {
    const char* e = getenv(name);
    if (!e || !*e) return 0;
    char host[128]; int port = PMP_PORT;
    if (*e == '[') { const char* c = strchr(e, ']'); if (!c) return 0; snprintf(host, sizeof host, "%.*s", (int)(c - e - 1), e + 1); if (c[1] == ':') port = atoi(c + 2); }
    else if (family == AF_INET && strchr(e, ':')) { const char* c = strchr(e, ':'); snprintf(host, sizeof host, "%.*s", (int)(c - e), e); port = atoi(c + 1); }
    else snprintf(host, sizeof host, "%s", e);
    memset(out, 0, sizeof *out);
    if (family == AF_INET) {
        struct sockaddr_in* a = (struct sockaddr_in*)out;
        a->sin_family = AF_INET; a->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &a->sin_addr) != 1) return 0;
        *len = sizeof *a;
    } else {
        struct sockaddr_in6* a = (struct sockaddr_in6*)out;
        a->sin6_family = AF_INET6; a->sin6_port = htons((uint16_t)port);
        if (inet_pton(AF_INET6, host, &a->sin6_addr) != 1) return 0;
        *len = sizeof *a;
    }
    return 1;
}

static int gateway4(struct sockaddr_storage* out, socklen_t* len) {
    if (env_addr("DQIX_GATEWAY", AF_INET, out, len)) return 1;
    struct sockaddr_in* a = (struct sockaddr_in*)out;
    memset(out, 0, sizeof *out);
    a->sin_family = AF_INET; a->sin_port = htons(PMP_PORT); *len = sizeof *a;
#ifdef _WIN32
    MIB_IPFORWARDROW row;
    struct in_addr dst; inet_pton(AF_INET, "8.8.8.8", &dst);
    if (GetBestRoute(dst.s_addr, 0, &row) != NO_ERROR || row.dwForwardNextHop == 0) return 0;
    a->sin_addr.s_addr = row.dwForwardNextHop;
    return 1;
#else
    FILE* f = fopen("/proc/net/route", "r");
    if (!f) return 0;
    char line[256]; int ok = 0;
    while (fgets(line, sizeof line, f)) {
        char ifn[64]; unsigned dest, gw, flags;
        if (sscanf(line, "%63s %x %x %x", ifn, &dest, &gw, &flags) == 4 && dest == 0 && gw != 0 && (flags & 2)) { a->sin_addr.s_addr = gw; ok = 1; break; }
    }
    fclose(f);
    return ok;
#endif
}

static int gateway6(struct sockaddr_storage* out, socklen_t* len) {
    if (env_addr("DQIX_GATEWAY6", AF_INET6, out, len)) return 1;
    struct sockaddr_in6* a = (struct sockaddr_in6*)out;
    memset(out, 0, sizeof *out);
    a->sin6_family = AF_INET6; a->sin6_port = htons(PMP_PORT); *len = sizeof *a;
#ifdef _WIN32
    MIB_IPFORWARD_ROW2 row; SOCKADDR_INET dst, src;
    memset(&dst, 0, sizeof dst);
    dst.Ipv6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "2001:4860:4860::8888", &dst.Ipv6.sin6_addr);
    if (GetBestRoute2(NULL, 0, NULL, &dst, 0, &row, &src) != NO_ERROR) return 0;
    static const uint8_t zero[16];
    if (!memcmp(&row.NextHop.Ipv6.sin6_addr, zero, 16)) return 0;
    a->sin6_addr = row.NextHop.Ipv6.sin6_addr;
    a->sin6_scope_id = row.InterfaceIndex;
    return 1;
#else
    FILE* f = fopen("/proc/net/ipv6_route", "r");
    if (!f) return 0;
    char line[300]; int ok = 0;
    while (fgets(line, sizeof line, f)) {
        char dst[40], dl[4], src[40], sl[4], nh[40], ifn[64]; unsigned metric, ref, use, flags;
        if (sscanf(line, "%39s %3s %39s %3s %39s %x %x %x %x %63s", dst, dl, src, sl, nh, &metric, &ref, &use, &flags, ifn) != 10) continue;
        if (strcmp(dl, "00") || strspn(dst, "0") != 32 || strspn(nh, "0") == 32 || !strcmp(ifn, "lo")) continue;
        for (int i = 0; i < 16; i++) { unsigned v; sscanf(nh + 2 * i, "%2x", &v); a->sin6_addr.s6_addr[i] = (uint8_t)v; }
        a->sin6_scope_id = if_nametoindex(ifn);
        ok = 1; break;
    }
    fclose(f);
    return ok;
#endif
}

/* ---------------------------------------------------------------- one exchange */
/* Send `req` to the router and wait for an answer that `accept` likes, resending
 * like the RFCs ask (250 ms, then doubling). Returns the answer's length or 0. */
typedef int (*Accept)(const uint8_t* r, int n, const void* ctx);
static int exchange(const struct sockaddr_storage* to, socklen_t tl, const struct sockaddr_storage* from, socklen_t fl,
                    const uint8_t* req, int rn, uint8_t* ans, int cap, Accept accept, const void* ctx, char* local, size_t ln) {
    sock_t s = socket(to->ss_family, SOCK_DGRAM, 0);
    if (s == BADSOCK) return 0;
    if (from && bind(s, (const struct sockaddr*)from, fl) != 0) { closesock(s); return 0; }
    if (connect(s, (const struct sockaddr*)to, tl) != 0) { closesock(s); return 0; }
    if (local) {
        struct sockaddr_storage me; socklen_t ml = sizeof me;
        local[0] = 0;
        if (getsockname(s, (struct sockaddr*)&me, &ml) == 0) {
            if (me.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in*)&me)->sin_addr, local, (socklen_t)ln);
            else inet_ntop(AF_INET6, &((struct sockaddr_in6*)&me)->sin6_addr, local, (socklen_t)ln);
        }
    }
    int got = 0;
    for (int wait = 250; wait <= 1000 && !got; wait *= 2) {        /* 3 tries, 1.75 s in all */
        send(s, (const char*)req, rn, 0);
        set_timeout(s, wait);
        double until = now_s() + wait / 1000.0;
        while (now_s() < until) {
            int r = (int)recv(s, (char*)ans, cap, 0);
            if (r <= 0) break;
            if (accept(ans, r, ctx)) { got = r; break; }
        }
    }
    closesock(s);
    return got;
}

/* ---------------------------------------------------------------- NAT-PMP */
static int pmp_ok(const uint8_t* r, int n, const void* ctx) {
    int op = *(const int*)ctx;
    return n >= 8 && r[0] == 0 && r[1] == (uint8_t)(0x80 | op);
}

/* op 0: the public address. 1 = ok */
static int pmp_address(const struct sockaddr_storage* gw, socklen_t gl, char* ext, size_t en, int* result) {
    uint8_t req[2] = { 0, 0 }, ans[64]; int op = 0;
    int n = exchange(gw, gl, NULL, 0, req, 2, ans, sizeof ans, pmp_ok, &op, NULL, 0);
    if (n < 12) { *result = -1; return 0; }
    *result = (int)get16(ans + 2);
    if (*result) return 0;
    snprintf(ext, en, "%u.%u.%u.%u", ans[8], ans[9], ans[10], ans[11]);
    return 1;
}

/* op 2: map TCP. lifetime 0 = take it away. Returns the granted lifetime (> 0) or 0. */
static uint32_t pmp_map(const struct sockaddr_storage* gw, socklen_t gl, int port, uint32_t lifetime, int* ext_port, int* result) {
    uint8_t req[12] = { 0, 2 }, ans[64]; int op = 2;
    put16(req + 4, (unsigned)port); put16(req + 6, lifetime ? (unsigned)port : 0); put32(req + 8, lifetime);
    int n = exchange(gw, gl, NULL, 0, req, sizeof req, ans, sizeof ans, pmp_ok, &op, NULL, 0);
    if (n < 16) { *result = -1; return 0; }
    *result = (int)get16(ans + 2);
    if (*result || get16(ans + 8) != (unsigned)port) return 0;
    if (ext_port) *ext_port = (int)get16(ans + 10);
    return lifetime ? (get32(ans + 12) ? get32(ans + 12) : 1) : 1;
}

/* ---------------------------------------------------------------- PCP */
typedef struct { uint8_t nonce[12]; int version_error; } PcpCtx;
static int pcp_ok(const uint8_t* r, int n, const void* vctx) {
    PcpCtx* c = (PcpCtx*)vctx;
    if (n >= 4 && r[0] == 0) { c->version_error = 1; return 1; }   /* a NAT-PMP-only router: "unsupported version" */
    if (n >= 4 && r[0] == 1 && r[3] == 1) { c->version_error = 1; return 1; }   /* PCP version 1 only */
    return n >= 60 && r[0] == 2 && r[1] == (0x80 | 1) && !memcmp(r + 24, c->nonce, 12);
}

static void client_ip(uint8_t out[16], const char* ip) {
    memset(out, 0, 16);
    struct in_addr a4; struct in6_addr a6;
    if (inet_pton(AF_INET, ip, &a4) == 1) { out[10] = out[11] = 0xFF; memcpy(out + 12, &a4, 4); }
    else if (inet_pton(AF_INET6, ip, &a6) == 1) memcpy(out, &a6, 16);
}

/* MAP TCP `port` for `ip` (this PC). Returns the granted lifetime or 0; *version_error
 * when the router speaks only NAT-PMP. */
static uint32_t pcp_map(const struct sockaddr_storage* gw, socklen_t gl, const struct sockaddr_storage* from, socklen_t fl,
                        const char* ip, int port, uint32_t lifetime, const uint8_t nonce[12],
                        char* ext, size_t en, int* ext_port, int* result, int* version_error) {
    uint8_t req[60], ans[1100];
    memset(req, 0, sizeof req);
    req[0] = 2; req[1] = 1;                                        /* version 2, MAP */
    put32(req + 4, lifetime);
    client_ip(req + 8, ip);
    memcpy(req + 24, nonce, 12);
    req[36] = 6;                                                   /* TCP */
    put16(req + 40, (unsigned)port); put16(req + 42, (unsigned)port);
    if (strchr(ip, ':')) memcpy(req + 44, req + 8, 16);            /* IPv6: the same address outside */
    else { req[54] = req[55] = 0xFF; }                             /* IPv4: any (::ffff:0.0.0.0) */
    PcpCtx c; memcpy(c.nonce, nonce, 12); c.version_error = 0;
    int n = exchange(gw, gl, from, fl, req, sizeof req, ans, sizeof ans, pcp_ok, &c, NULL, 0);
    *version_error = c.version_error;
    if (!n || c.version_error) { *result = -1; return 0; }
    *result = ans[3];
    if (*result) return 0;
    if (ext_port) *ext_port = (int)get16(ans + 42);
    if (ext) {
        const uint8_t* x = ans + 44;
        static const uint8_t mapped[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
        if (!memcmp(x, mapped, 12)) snprintf(ext, en, "%u.%u.%u.%u", x[12], x[13], x[14], x[15]);
        else inet_ntop(AF_INET6, x, ext, (socklen_t)en);
    }
    return lifetime ? (get32(ans + 4) ? get32(ans + 4) : 1) : 1;
}

/* ---------------------------------------------------------------- what is open */
typedef struct {
    int open, pcp, v6, port;
    struct sockaddr_storage gw, from; socklen_t gl, fl;
    char ip[64];
    uint8_t nonce[12];
    double renew_at;
} Mapping;
static Mapping g_m4, g_m6;

static void make_nonce(uint8_t n[12]) {
    static unsigned seed;
    if (!seed) seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)&seed;
    for (int i = 0; i < 12; i++) { seed = seed * 1103515245u + 12345u; n[i] = (uint8_t)(seed >> 16); }
}

static const char* pcp_reason(int r) {
    switch (r) {
    case 2: return "not allowed for this PC";
    case 7: return "PCP is switched off for port openings";
    case 8: return "the router is out of room";
    case 11: return "the router can't open this port";
    case 12: return "the router can't open this port";
    default: return "the router said no";
    }
}

static int is_private4(const char* ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    return sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
           (a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b < 32) || (a == 100 && b >= 64 && b < 128) || a == 0);
}

int portmap_open(int port, char* ext_ip, size_t en, int* ext_port, char* method, size_t mn, char* why, size_t wn) {
    ext_ip[0] = 0; method[0] = 0; *ext_port = port;
    Mapping* m = &g_m4;
    memset(m, 0, sizeof *m);
    if (!gateway4(&m->gw, &m->gl)) { snprintf(why, wn, "couldn't find the router"); return 0; }
    /* this PC's address towards the router */
    {
        sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s != BADSOCK) {
            if (connect(s, (struct sockaddr*)&m->gw, m->gl) == 0) {
                struct sockaddr_in me; socklen_t ml = sizeof me;
                if (getsockname(s, (struct sockaddr*)&me, &ml) == 0) inet_ntop(AF_INET, &me.sin_addr, m->ip, sizeof m->ip);
            }
            closesock(s);
        }
    }
    if (!m->ip[0]) { snprintf(why, wn, "couldn't reach the router"); return 0; }
    make_nonce(m->nonce);
    int result = -1, verr = 0, ep = port;
    char ext[64] = "";
    uint32_t life = pcp_map(&m->gw, m->gl, NULL, 0, m->ip, port, LIFETIME, m->nonce, ext, sizeof ext, &ep, &result, &verr);
    if (life) { m->pcp = 1; snprintf(method, mn, "PCP"); }
    else {
        int r2 = -1;
        life = pmp_map(&m->gw, m->gl, port, LIFETIME, &ep, &r2);
        if (life) { snprintf(method, mn, "NAT-PMP"); int ra; pmp_address(&m->gw, m->gl, ext, sizeof ext, &ra); }
        else {
            if (result > 0) snprintf(why, wn, "%s (PCP)", pcp_reason(result));
            else if (r2 > 0) snprintf(why, wn, "the router refused (NAT-PMP error %d)", r2);
            else snprintf(why, wn, "the router doesn't answer NAT-PMP or PCP either");
            return 0;
        }
    }
    m->open = 1; m->port = port;
    m->renew_at = now_s() + life / 2.0;
    snprintf(ext_ip, en, "%s", ext);
    *ext_port = ep;
    if (!ext[0] || is_private4(ext)) {
        if (ext[0]) snprintf(why, wn, "the router's own internet address is private (%s): your provider shares one address between customers, "
                             "so friends can't reach you this way", ext);
        else snprintf(why, wn, "the router opened the port but didn't say its internet address");
        return 2;
    }
    return 1;
}

int portmap_open6(const char* v6, int port, char* why, size_t wn) {
    Mapping* m = &g_m6;
    memset(m, 0, sizeof *m);
    if (!gateway6(&m->gw, &m->gl)) { snprintf(why, wn, "couldn't find the IPv6 router"); return 0; }
    /* send from the global address itself: PCP checks that it matches the request */
    struct sockaddr_in6* f = (struct sockaddr_in6*)&m->from;
    memset(f, 0, sizeof *f);
    f->sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, v6, &f->sin6_addr) != 1) { snprintf(why, wn, "odd IPv6 address"); return 0; }
    m->fl = sizeof *f;
    snprintf(m->ip, sizeof m->ip, "%s", v6);
    make_nonce(m->nonce);
    int result = -1, verr = 0, ep = port;
    uint32_t life = pcp_map(&m->gw, m->gl, &m->from, m->fl, v6, port, LIFETIME, m->nonce, NULL, 0, &ep, &result, &verr);
    if (!life) {
        if (verr) snprintf(why, wn, "the router doesn't support PCP");
        else if (result > 0) snprintf(why, wn, "%s (PCP)", pcp_reason(result));
        else snprintf(why, wn, "the router doesn't answer PCP");
        return 0;
    }
    m->open = 1; m->pcp = 1; m->v6 = 1; m->port = port;
    m->renew_at = now_s() + life / 2.0;
    return 1;
}

static void renew(Mapping* m, uint32_t lifetime) {
    int result, verr, ep;
    uint32_t life;
    if (m->pcp) life = pcp_map(&m->gw, m->gl, m->v6 ? &m->from : NULL, m->v6 ? m->fl : 0, m->ip, m->port, lifetime, m->nonce, NULL, 0, &ep, &result, &verr);
    else life = pmp_map(&m->gw, m->gl, m->port, lifetime, &ep, &result);
    if (!lifetime) { m->open = 0; return; }
    m->renew_at = now_s() + (life ? life / 2.0 : 60.0);       /* no answer: try again in a minute */
}

void portmap_renew(void) {
    double t = now_s();
    if (g_m4.open && t >= g_m4.renew_at) renew(&g_m4, LIFETIME);
    if (g_m6.open && t >= g_m6.renew_at) renew(&g_m6, LIFETIME);
}

void portmap_close(void) {
    if (g_m4.open) renew(&g_m4, 0);
    if (g_m6.open) renew(&g_m6, 0);
}

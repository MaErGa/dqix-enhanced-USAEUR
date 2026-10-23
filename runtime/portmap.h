/* NAT-PMP (RFC 6886) and PCP (RFC 6887): the other two ways, besides UPnP,
 * of asking the home router to let friends in. Many routers that have UPnP
 * switched off (or never had it: Apple's, some providers' boxes) answer one of
 * these. All calls block for up to a few seconds. */
#pragma once
#include <stddef.h>

/* IPv4: open TCP `port` on the router to this PC. 1: open, ext_ip:ext_port is
 * the public address; 2: open, but the router's own address is private
 * (the provider shares one address between customers); 0: failed. method gets
 * "PCP" or "NAT-PMP"; why says why it failed. */
int portmap_open(int port, char* ext_ip, size_t en, int* ext_port, char* method, size_t mn, char* why, size_t wn);

/* IPv6: ask the router's firewall (PCP) to let connections to `v6` (this PC's
 * global address), TCP `port`, in. 1: allowed. */
int portmap_open6(const char* v6, int port, char* why, size_t wn);

void portmap_renew(void);   /* call often: renews what is open before it runs out */
void portmap_close(void);   /* take it all away again */

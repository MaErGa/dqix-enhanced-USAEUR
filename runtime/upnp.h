/* Opening a port on the home router for the host of a session (UPnP). */
#pragma once
#include <stddef.h>

/* 1: open, ext_ip = the public address; 2: open but the public address is
 * itself private (carrier-grade NAT); 0: failed (why says why). lan_ip =
 * this PC on the home network. Blocks for up to a few seconds. */
int upnp_open(int port, char* ext_ip, size_t en, char* lan_ip, size_t ln, char* why, size_t wn);
void upnp_close(void);                 /* take the forwarding (and the IPv6 pinhole) away again */
/* IPv6: open the router's firewall for v6 (this PC), TCP port (UPnP IGD 2). Only
 * after upnp_open has found the router. 1: open; why says why not. */
int upnp_pinhole(const char* v6, int port, char* why, size_t wn);
void upnp_renew(void);                 /* keep the pinhole open: call every so often */

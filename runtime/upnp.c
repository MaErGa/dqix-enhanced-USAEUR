/* Asking the home router to let friends in (UPnP Internet Gateway Device):
 * find the router (SSDP), learn the public address, forward a TCP port to
 * this PC. Most home routers allow this; if not, the host forwards the port
 * by hand or everyone uses a virtual LAN (see the session help). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upnp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define BADSOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BADSOCK (-1)
#define closesock close
#endif

static char g_control[512], g_service[128], g_host[128];
static int g_port_open, g_ext_port, g_router_port;
static char g_fw_control[512];                   /* WANIPv6FirewallControl: opening the router's IPv6 firewall */
static char g_pinhole[32];                       /* the open pinhole's id */
static const char* FW_SERVICE = "urn:schemas-upnp-org:service:WANIPv6FirewallControl:1";

static void set_timeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = { ms / 1000, (ms % 1000) * 1000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
}

/* http://host:port/path -> parts */
static int split_url(const char* url, char* host, size_t hn, int* port, char* path, size_t pn) {
    if (strncmp(url, "http://", 7)) return 0;
    const char* h = url + 7;
    const char* slash = strchr(h, '/');
    const char* colon = strchr(h, ':');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    *port = 80;
    if (colon && (!slash || colon < slash)) { *port = atoi(colon + 1); hl = (size_t)(colon - h); }
    snprintf(host, hn, "%.*s", (int)hl, h);
    snprintf(path, pn, "%s", slash ? slash : "/");
    return 1;
}

/* one HTTP request, the whole answer in `out` (body after the headers). Returns its length or -1.
 * `local` gets this PC's address on the router's network. */
static int http(const char* host, int port, const char* req, char* out, int cap, char* local, size_t ln) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BADSOCK) { freeaddrinfo(res); return -1; }
    set_timeout(s, 3000);
    int ok = connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) { closesock(s); return -1; }
    if (local) {
        struct sockaddr_in a; socklen_t al = sizeof a;
        if (getsockname(s, (struct sockaddr*)&a, &al) == 0) inet_ntop(AF_INET, &a.sin_addr, local, (socklen_t)ln);
    }
    send(s, req, (int)strlen(req), 0);
    int n = 0;
    for (;;) {
        int r = (int)recv(s, out + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += r;
        if (n >= cap - 1) break;
    }
    closesock(s);
    out[n] = 0;
    return n;
}

/* the text between <tag> and </tag> after `from` */
static int xml_tag(const char* from, const char* tag, char* out, size_t n) {
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag); snprintf(close, sizeof close, "</%s>", tag);
    const char* a = strstr(from, open);
    if (!a) {                                                       /* <ns:tag> */
        snprintf(open, sizeof open, ":%s>", tag);
        a = strstr(from, open);
        if (!a) return 0;
    }
    a += strlen(open);
    const char* b = strstr(a, "</");
    if (!b) return 0;
    snprintf(out, n, "%.*s", (int)(b - a), a);
    (void)close;
    return 1;
}

static int find_router(char* location, size_t n) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BADSOCK) return 0;
    set_timeout(s, 600);
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    static const char* targets[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                     "urn:schemas-upnp-org:service:WANIPConnection:1",
                                     "urn:schemas-upnp-org:device:InternetGatewayDevice:2" };
    char buf[2048];
    for (int round = 0; round < 4; round++) {
        for (int t = 0; t < 3; t++) {
            snprintf(buf, sizeof buf, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nST: %s\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n", targets[t]);
            sendto(s, buf, (int)strlen(buf), 0, (struct sockaddr*)&to, sizeof to);
        }
        for (;;) {
            int r = (int)recv(s, buf, sizeof buf - 1, 0);
            if (r <= 0) break;
            buf[r] = 0;
            for (char* p = buf; *p; p++) {
                if ((p == buf || p[-1] == '\n') && (!strncmp(p, "LOCATION:", 9) || !strncmp(p, "Location:", 9) || !strncmp(p, "location:", 9))) {
                    p += 9; while (*p == ' ') p++;
                    char* e = p; while (*e && *e != '\r' && *e != '\n') e++;
                    snprintf(location, n, "%.*s", (int)(e - p), p);
                    closesock(s);
                    return 1;
                }
            }
        }
    }
    closesock(s);
    return 0;
}

static int soap_to(const char* control, const char* service, const char* action, const char* args, char* out, int cap, char* local, size_t ln) {
    char host[128], path[400]; int port;
    if (!split_url(control, host, sizeof host, &port, path, sizeof path)) return -1;
    char body[1500];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, service, args, action);
    char req[2500];
    snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHOST: %s:%d\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nCONTENT-LENGTH: %d\r\n"
             "SOAPACTION: \"%s#%s\"\r\nConnection: close\r\n\r\n%s", path, host, port, (int)strlen(body), service, action, body);
    return http(host, port, req, out, cap, local, ln);
}
static int soap(const char* action, const char* args, char* out, int cap, char* local, size_t ln) {
    return soap_to(g_control, g_service, action, args, out, cap, local, ln);
}

int upnp_open(int port, char* ext_ip, size_t en, char* lan_ip, size_t ln, char* why, size_t wn) {
    ext_ip[0] = lan_ip[0] = 0;
    char loc[512];
    const char* test_loc = getenv("DQIX_UPNP_LOCATION");             /* tests: a stand-in router's description */
    if (test_loc) snprintf(loc, sizeof loc, "%s", test_loc);
    else if (!find_router(loc, sizeof loc)) { snprintf(why, wn, "no router answered (UPnP is off or not supported)"); return 0; }
    char host[128], path[400]; int hport;
    if (!split_url(loc, host, sizeof host, &hport, path, sizeof path)) { snprintf(why, wn, "the router gave an odd address"); return 0; }
    static char xml[65536];
    char req[700];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHOST: %s:%d\r\nConnection: close\r\n\r\n", path, host, hport);
    if (http(host, hport, req, xml, sizeof xml, lan_ip, ln) <= 0) { snprintf(why, wn, "the router didn't describe itself"); return 0; }
    {   /* IGD 2 routers may also open their IPv6 firewall (upnp_pinhole) */
        const char* fw = strstr(xml, FW_SERVICE);
        char c[400];
        g_fw_control[0] = 0;
        if (fw && xml_tag(fw, "controlURL", c, sizeof c)) {
            if (!strncmp(c, "http://", 7)) snprintf(g_fw_control, sizeof g_fw_control, "%s", c);
            else snprintf(g_fw_control, sizeof g_fw_control, "http://%s:%d%s%s", host, hport, c[0] == '/' ? "" : "/", c);
        }
    }
    /* the WAN connection service and its control address */
    const char* svc = NULL;
    static const char* kinds[] = { "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                   "urn:schemas-upnp-org:service:WANPPPConnection:1" };
    for (int i = 0; i < 3 && !svc; i++) { svc = strstr(xml, kinds[i]); if (svc) snprintf(g_service, sizeof g_service, "%s", kinds[i]); }
    if (!svc) { snprintf(why, wn, "the router can't forward ports"); return 0; }
    char ctl[400];
    if (!xml_tag(svc, "controlURL", ctl, sizeof ctl)) { snprintf(why, wn, "the router's description is incomplete"); return 0; }
    if (!strncmp(ctl, "http://", 7)) snprintf(g_control, sizeof g_control, "%s", ctl);
    else snprintf(g_control, sizeof g_control, "http://%s:%d%s%s", host, hport, ctl[0] == '/' ? "" : "/", ctl);
    snprintf(g_host, sizeof g_host, "%s", host); g_router_port = hport;

    static char ans[16384];
    if (soap("GetExternalIPAddress", "", ans, sizeof ans, NULL, 0) > 0) xml_tag(ans, "NewExternalIPAddress", ext_ip, en);
    char args[800];
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>Dragon Quest IX PC</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>",
             port, port, lan_ip);
    int n = soap("AddPortMapping", args, ans, sizeof ans, NULL, 0);
    if (n <= 0 || !strstr(ans, " 200 ")) {
        char code[32] = "", desc[96] = "";
        xml_tag(ans, "errorCode", code, sizeof code); xml_tag(ans, "errorDescription", desc, sizeof desc);
        snprintf(why, wn, "the router refused to forward the port%s%s%s%s", code[0] ? " (" : "", code, desc[0] ? " " : "", desc[0] ? desc : "");
        if (code[0]) strncat(why, ")", wn - strlen(why) - 1);
        return 0;
    }
    g_port_open = 1; g_ext_port = port;
    unsigned a = 0, b = 0, c = 0, d = 0;
    int priv = sscanf(ext_ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
               (a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b < 32) || (a == 100 && b >= 64 && b < 128) || a == 0);
    if (priv) {
        snprintf(why, wn, "the router's own internet address is private (%s): your provider shares one address between customers, "
                 "so friends can't reach you this way", ext_ip);
        return 2;
    }
    return 1;
}

static void pinhole_close(void);
void upnp_close(void) {
    pinhole_close();
    if (!g_port_open) return;
    char args[300], ans[4096];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>", g_ext_port);
    soap("DeletePortMapping", args, ans, sizeof ans, NULL, 0);
    g_port_open = 0;
}

/* IPv6: open the router's firewall for this PC's address and port (UPnP IGD 2,
 * WANIPv6FirewallControl). Only after upnp_open found the router. 1: open. */
int upnp_pinhole(const char* v6, int port, char* why, size_t wn) {
    if (!g_fw_control[0]) { snprintf(why, wn, "the router has no UPnP IPv6 firewall control"); return 0; }
    char args[600]; static char ans[8192];
    snprintf(args, sizeof args, "<RemoteHost></RemoteHost><RemotePort>0</RemotePort><InternalClient>%s</InternalClient>"
             "<InternalPort>%d</InternalPort><Protocol>6</Protocol><LeaseTime>7200</LeaseTime>", v6, port);
    int n = soap_to(g_fw_control, FW_SERVICE, "AddPinhole", args, ans, sizeof ans, NULL, 0);
    if (n <= 0 || !strstr(ans, " 200 ") || !xml_tag(ans, "UniqueID", g_pinhole, sizeof g_pinhole)) {
        char code[32] = "";
        if (n > 0) xml_tag(ans, "errorCode", code, sizeof code);
        snprintf(why, wn, "the router refused to open its IPv6 firewall%s%s%s", code[0] ? " (error " : "", code, code[0] ? ")" : "");
        g_pinhole[0] = 0;
        return 0;
    }
    return 1;
}

/* keep the pinhole open (its lease is 2 hours): call every so often */
void upnp_renew(void) {
    if (!g_pinhole[0]) return;
    char args[200]; static char ans[4096];
    snprintf(args, sizeof args, "<UniqueID>%s</UniqueID><NewLeaseTime>7200</NewLeaseTime>", g_pinhole);
    soap_to(g_fw_control, FW_SERVICE, "UpdatePinhole", args, ans, sizeof ans, NULL, 0);
}

static void pinhole_close(void) {
    if (!g_pinhole[0]) return;
    char args[200]; static char ans[4096];
    snprintf(args, sizeof args, "<UniqueID>%s</UniqueID>", g_pinhole);
    soap_to(g_fw_control, FW_SERVICE, "DeletePinhole", args, ans, sizeof ans, NULL, 0);
    g_pinhole[0] = 0;
}

// wsock32 on BSD sockets: Diablo II's TCP/IP games (Fog's game server listens on port 4000, D2Net connects).
// A Windows SOCKET is the host fd itself. Windows structures are converted where they differ: fd_set (a
// count and an array of handles), timeval (two 32-bit longs), hostent (32-bit pointers in guest memory);
// sockaddr_in has the same 16-byte layout on both. Errors map from errno to the WSA codes the game checks.
#include "native.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define W "wsock32.dll"
#define SOCKET_ERROR 0xFFFFFFFFu
static __thread u32 g_wsa_err;
static int g_wsa_refs;
static char g_hostname[64] = "d2host";  // what gethostname reports; gethostbyname of it lists our LAN addresses

static u32 wsa_errno(int e) {
    static const struct { int e; u32 w; } map[] = {
        {EWOULDBLOCK, 10035}, {EINPROGRESS, 10035}, {EALREADY, 10037}, {ENOTSOCK, 10038}, {EMSGSIZE, 10040},
        {EADDRINUSE, 10048}, {EADDRNOTAVAIL, 10049}, {ENETDOWN, 10050}, {ENETUNREACH, 10051}, {ECONNABORTED, 10053},
        {ECONNRESET, 10054}, {EPIPE, 10054}, {ENOBUFS, 10055}, {EISCONN, 10056}, {ENOTCONN, 10057},
        {ETIMEDOUT, 10060}, {ECONNREFUSED, 10061}, {EHOSTUNREACH, 10065}, {EINVAL, 10022}, {EBADF, 10009},
        {EINTR, 10004}, {EACCES, 10013}, {EAFNOSUPPORT, 10047}, {EMFILE, 10024},
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; ++i) if (map[i].e == e) return map[i].w;
    return 10022;
}
// a host call's result: -1 sets the WSA error from errno (non-blocking connect: Windows says WOULDBLOCK)
static u32 res(long r) { if (r < 0) { g_wsa_err = wsa_errno(errno); return SOCKET_ERROR; } return (u32)r; }

static void nt_WSAStartup(Cpu* c) {
    u32 p = ARG(1);
    if (p) { memset(H(p), 0, 400); wr16(p, 0x0101); wr16(p + 2, 0x0202); strcpy((char*)H(p + 4), "d2recomp"); wr16(p + 390, 32767); }
    g_wsa_refs++;
    RET(0, 2);
}
static void nt_WSACleanup(Cpu* c) { if (g_wsa_refs <= 0) { g_wsa_err = 10093; RET(SOCKET_ERROR, 0); } g_wsa_refs--; RET(0, 0); }
static void nt_WSAGetLastError(Cpu* c) { RET(g_wsa_err, 0); }
static void nt_WSASetLastError(Cpu* c) { g_wsa_err = ARG(0); RET(0, 1); }

static void nt_socket(Cpu* c) {
    int fd = socket((int)ARG(0), (int)ARG(1), (int)ARG(2));
    if (fd >= 0) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
    LOG("net: socket(%u, %u, %u) = %d", ARG(0), ARG(1), ARG(2), fd);
    RET(res(fd), 3);
}
static int netlog(void) { static int on = -1; if (on < 0) on = getenv("D2NETLOG") != NULL; return on; }
static void nt_closesocket(Cpu* c) { LOG("net: closesocket(%d)", (int)ARG(0)); RET(res(close((int)ARG(0))), 1); }
static void nt_bind(Cpu* c) {
    struct sockaddr_in a; memcpy(&a, H(ARG(1)), sizeof a);
    LOG("net: bind(%d, %s:%u)", (int)ARG(0), inet_ntoa(a.sin_addr), ntohs(a.sin_port));
    RET(res(bind((int)ARG(0), (struct sockaddr*)&a, sizeof a)), 3);
}
static void nt_listen(Cpu* c) { RET(res(listen((int)ARG(0), (int)ARG(1))), 2); }
static void nt_accept(Cpu* c) {
    struct sockaddr_in a; socklen_t n = sizeof a;
    int fd = accept((int)ARG(0), (struct sockaddr*)&a, &n);
    if (fd >= 0) {
        LOG("net: accepted %d from %s:%u", fd, inet_ntoa(a.sin_addr), ntohs(a.sin_port));
        if (ARG(1)) memcpy(H(ARG(1)), &a, sizeof a);
        if (ARG(2)) wr32(ARG(2), sizeof a);
    }
    RET(res(fd), 3);
}
static void nt_connect(Cpu* c) {
    struct sockaddr_in a; memcpy(&a, H(ARG(1)), sizeof a);
    long r = connect((int)ARG(0), (struct sockaddr*)&a, sizeof a);
    LOG("net: connect(%d, %s:%u) = %ld errno %d", (int)ARG(0), inet_ntoa(a.sin_addr), ntohs(a.sin_port), r, r < 0 ? errno : 0);
    RET(res(r), 3);
}
static void nt_send(Cpu* c) {
    long r = send((int)ARG(0), H(ARG(1)), (size_t)ARG(2), (int)(ARG(3) & 3) | MSG_NOSIGNAL);
    if (netlog()) LOG("net: send(%d, %u) = %ld errno %d first %02X", (int)ARG(0), ARG(2), r, r < 0 ? errno : 0, ARG(2) ? rd8(ARG(1)) : 0);
    RET(res(r), 4);
}
static void nt_recv(Cpu* c) {
    long r = recv((int)ARG(0), H(ARG(1)), (size_t)ARG(2), (int)(ARG(3) & 3));
    if (netlog() && !(r < 0 && errno == EAGAIN)) LOG("net: recv(%d, %u) = %ld errno %d first %02X", (int)ARG(0), ARG(2), r, r < 0 ? errno : 0, r > 0 ? rd8(ARG(1)) : 0);
    RET(res(r), 4);
}

// Windows fd_set: u32 count, then up to 64 SOCKETs
static int fdset_in(u32 g, fd_set* s, int* maxfd) {
    FD_ZERO(s);
    if (!g) return 0;
    u32 n = rd32(g);
    for (u32 i = 0; i < n && i < 64; ++i) {
        int fd = (int)rd32(g + 4 + 4 * i);
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        FD_SET(fd, s);
        if (fd > *maxfd) *maxfd = fd;
    }
    return 1;
}
static void fdset_out(u32 g, fd_set* s) {  // keep only the ready ones, as Windows does
    if (!g) return;
    u32 n = rd32(g), k = 0;
    for (u32 i = 0; i < n && i < 64; ++i) {
        u32 fd = rd32(g + 4 + 4 * i);
        if ((int)fd >= 0 && (int)fd < FD_SETSIZE && FD_ISSET((int)fd, s)) wr32(g + 4 + 4 * k++, fd);
    }
    wr32(g, k);
}
static void nt_select(Cpu* c) {
    fd_set r, w, e;
    int maxfd = -1;
    fdset_in(ARG(1), &r, &maxfd); fdset_in(ARG(2), &w, &maxfd); fdset_in(ARG(3), &e, &maxfd);
    struct timeval tv, *ptv = NULL;
    if (ARG(4)) { tv.tv_sec = (s32)rd32(ARG(4)); tv.tv_usec = (s32)rd32(ARG(4) + 4); ptv = &tv; }
    int n = select(maxfd + 1, ARG(1) ? &r : NULL, ARG(2) ? &w : NULL, ARG(3) ? &e : NULL, ptv);
    if (n >= 0) { fdset_out(ARG(1), &r); fdset_out(ARG(2), &w); fdset_out(ARG(3), &e); }
    if (netlog() && n != 0) LOG("net: select(r %u w %u e %u, %s) = %d", ARG(1) ? rd32(ARG(1)) : 0, ARG(2) ? rd32(ARG(2)) : 0, ARG(3) ? rd32(ARG(3)) : 0, ptv ? "timeout" : "block", n);
    RET(res(n), 5);
}
static void nt___WSAFDIsSet(Cpu* c) {
    u32 fd = ARG(0), g = ARG(1), n = rd32(g);
    for (u32 i = 0; i < n && i < 64; ++i) if (rd32(g + 4 + 4 * i) == fd) RET(1, 2);
    RET(0, 2);
}

static void nt_setsockopt(Cpu* c) {
    int fd = (int)ARG(0), level = (int)ARG(1), name = (int)ARG(2), len = (int)ARG(4);
    u32 val = ARG(3);
    int v = val && len >= 4 ? (int)rd32(val) : 0;
    if (level == 0xFFFF) {  // SOL_SOCKET
        static const struct { int w, l; } opt[] = {
            {0x0004, SO_REUSEADDR}, {0x0008, SO_KEEPALIVE}, {0x0020, SO_BROADCAST}, {0x1001, SO_SNDBUF}, {0x1002, SO_RCVBUF},
        };
        for (unsigned i = 0; i < sizeof opt / sizeof *opt; ++i)
            if (opt[i].w == name) RET(res(setsockopt(fd, SOL_SOCKET, opt[i].l, &v, sizeof v)), 5);
        if (name == 0x0080 && val && len >= 4) {  // SO_LINGER: two u16 on Windows
            struct linger l = {rd16(val), rd16(val + 2)};
            RET(res(setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l)), 5);
        }
        if (name == (int)~0x0080u) { struct linger l = {0, 0}; RET(res(setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l)), 5); }
    }
    if (level == 6 && name == 1) RET(res(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v)), 5);
    LOG("net: setsockopt(%d, 0x%x, 0x%x) ignored", fd, level, name);
    RET(0, 5);
}
static void nt_ioctlsocket(Cpu* c) {
    int fd = (int)ARG(0);
    u32 cmd = ARG(1), argp = ARG(2);
    if (cmd == 0x8004667Eu) {  // FIONBIO
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0) RET(res(fl), 3);
        RET(res(fcntl(fd, F_SETFL, rd32(argp) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK)), 3);
    }
    if (cmd == 0x4004667Fu) {  // FIONREAD
        int n = 0;
        long r = ioctl(fd, FIONREAD, &n);
        if (r >= 0) wr32(argp, (u32)n);
        RET(res(r), 3);
    }
    LOG("net: ioctlsocket(%d, 0x%x) unsupported", fd, cmd);
    g_wsa_err = 10022;
    RET(SOCKET_ERROR, 3);
}
static void nt_getpeername(Cpu* c) {
    struct sockaddr_in a; socklen_t n = sizeof a;
    long r = getpeername((int)ARG(0), (struct sockaddr*)&a, &n);
    if (r >= 0) { memcpy(H(ARG(1)), &a, sizeof a); wr32(ARG(2), sizeof a); }
    RET(res(r), 3);
}
static void nt_getsockname(Cpu* c) {
    struct sockaddr_in a; socklen_t n = sizeof a;
    long r = getsockname((int)ARG(0), (struct sockaddr*)&a, &n);
    if (r >= 0) { memcpy(H(ARG(1)), &a, sizeof a); wr32(ARG(2), sizeof a); }
    RET(res(r), 3);
}

// hostent in guest memory: name, aliases, type/length, then a NULL-terminated address list
static u32 hostent_of(const char* name, const u32* addrs, int n) {
    static u32 he;
    if (!he) he = galloc(512);
    u32 nm = he + 16, al = nm + 64, list = al + 4, data = list + 4 * 17;
    if (n > 16) n = 16;
    snprintf((char*)H(nm), 64, "%s", name);
    wr32(al, 0);
    for (int i = 0; i < n; ++i) { wr32(data + 4 * i, addrs[i]); wr32(list + 4 * i, data + 4 * i); }
    wr32(list + 4 * n, 0);
    wr32(he, nm); wr32(he + 4, al); wr16(he + 8, AF_INET); wr16(he + 10, 4); wr32(he + 12, list);
    return he;
}
static void nt_gethostbyname(Cpu* c) {
    const char* name = gstr(ARG(0));
    u32 addrs[16];
    int n = 0;
    if (!strcasecmp(name, g_hostname)) {
        // our own name: the LAN addresses (the host's game screen shows them to the players who join)
        struct ifaddrs* ifs;
        if (!getifaddrs(&ifs)) {
            for (struct ifaddrs* i = ifs; i && n < 16; i = i->ifa_next)
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(ntohl(((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr) >> 24 == 127))
                    addrs[n++] = ((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr;
            freeifaddrs(ifs);
        }
        if (!n) addrs[n++] = htonl(INADDR_LOOPBACK);
    } else {
        struct addrinfo hint = {0}, *ai;
        hint.ai_family = AF_INET;
        if (!getaddrinfo(name, NULL, &hint, &ai)) {
            for (struct addrinfo* p = ai; p && n < 16; p = p->ai_next) addrs[n++] = ((struct sockaddr_in*)p->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(ai);
        }
    }
    LOG("net: gethostbyname(%s) -> %d addresses", name, n);
    if (!n) { g_wsa_err = 11001; RET(0, 1); }
    RET(hostent_of(name, addrs, n), 1);
}
static void nt_gethostname(Cpu* c) {
    if (ARG(1) <= strlen(g_hostname)) { g_wsa_err = 10014; RET(SOCKET_ERROR, 2); }
    strcpy((char*)H(ARG(0)), g_hostname);
    RET(0, 2);
}
static void nt_htons(Cpu* c) { RET(htons((u16)ARG(0)), 1); }
static void nt_ntohs(Cpu* c) { RET(ntohs((u16)ARG(0)), 1); }
static void nt_inet_addr(Cpu* c) { RET(inet_addr(gstr(ARG(0))), 1); }
static void nt_inet_ntoa(Cpu* c) {
    static u32 buf;
    if (!buf) buf = galloc(20);
    struct in_addr a = {ARG(0)};
    strcpy((char*)H(buf), inet_ntoa(a));
    RET(buf, 1);
}

#define E(n) {W, #n, 0, nt_##n}
NATIVE_TABLE(net,
    E(WSAStartup), E(WSACleanup), E(WSAGetLastError), E(WSASetLastError), E(__WSAFDIsSet), E(socket),
    E(closesocket), E(bind), E(listen), E(accept), E(connect), E(send), E(recv), E(select), E(setsockopt),
    E(ioctlsocket), E(getpeername), E(getsockname), E(gethostbyname), E(gethostname), E(htons), E(ntohs),
    E(inet_addr), E(inet_ntoa),
)

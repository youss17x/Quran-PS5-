// Quran PS5 — libc functions libcurl/OpenSSL reference that the native title runtime lacks.
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <sys/time.h>
#include <cstdint>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

extern "C" {

// curl's wakeup pipe
int pipe2(int fds[2], int flags) {
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if (flags & O_NONBLOCK) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        if (flags & O_CLOEXEC) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    return 0;
}

// IPv6 zone ids in URLs: not used
unsigned int if_nametoindex(const char *) { return 0; }

// .netrc lookup: no user database on the console
struct passwd;
int getpwuid_r(unsigned, struct passwd *, char *, size_t, struct passwd **result) {
    if (result) *result = nullptr;
    return ENOENT;
}

// OpenSSL DSO helper: not used for static builds
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info_shim;
int dladdr(const void *, Dl_info_shim *) { return 0; }

// OpenSSL async fibres (never started by curl)
int _setjmp(void *) { return 0; }
[[noreturn]] void _longjmp(void *, int) {
    fprintf(stderr, "_longjmp called (unsupported)\n");
    abort();
}

// UTC broken-down time (certificate dates)
struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    long long t = (long long)*tp;
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = (int)(rem / 3600); rem %= 3600;
    out->tm_min = (int)(rem / 60); out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((4 + days) % 7); if (out->tm_wday < 0) out->tm_wday += 7;
    // civil_from_days (Howard Hinnant)
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    out->tm_year = (int)(y - 1900); out->tm_mon = (int)m - 1; out->tm_mday = (int)d;
    static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    out->tm_yday = cum[m - 1] + (int)d - 1 + (leap && m > 2);
    out->tm_isdst = 0;
    return out;
}

// zstd tracing hooks (weak in libzstd; the native linker needs real definitions)
unsigned long long ZSTD_trace_compress_begin(const void *) { return 0; }
void ZSTD_trace_compress_end(unsigned long long, const void *) {}
unsigned long long ZSTD_trace_decompress_begin(const void *) { return 0; }
void ZSTD_trace_decompress_end(unsigned long long, const void *) {}


// ---------- DNS ----------
// getaddrinfo normally comes from libScePosixForWebKit, which a native title does not load.
// We resolve through libSceNet's resolver instead (IPv4).
// Minimal DNS client (A records over UDP) using plain kernel sockets, which work inside a title.
// The console's own resolver library (libSceNet) cannot be loaded from a native title here.
static std::mutex g_net_mutex;
static bool parse_ipv4(const char *s, struct in_addr *out);
void dns_log(const char *fmt, ...);
struct DnsCache { char host[128]; struct in_addr addr; };
static DnsCache g_dns_cache[8];

static int dns_query(const char *server, const char *host, struct in_addr *out) {
    unsigned char q[300]; int n = 0;
    unsigned short id = (unsigned short)(time(nullptr) ^ (uintptr_t)host);
    q[n++] = id >> 8; q[n++] = id & 0xFF;
    q[n++] = 0x01; q[n++] = 0x00;                 // recursion desired
    q[n++] = 0; q[n++] = 1;                       // 1 question
    for (int i = 0; i < 6; i++) q[n++] = 0;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + len + 6 > sizeof q) return -1;
        q[n++] = (unsigned char)len; memcpy(q + n, p, len); n += (int)len;
        p += len; if (*p == '.') p++;
    }
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;   // type A, class IN

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { dns_log("socket failed errno=%d", errno); return -1; }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa; sa.sin_family = AF_INET; sa.sin_port = htons(53);
    parse_ipv4(server, &sa.sin_addr);

    int result = -1;
    for (int attempt = 0; attempt < 2 && result != 0; attempt++) {
        if (sendto(fd, q, n, 0, (struct sockaddr *)&sa, sizeof sa) != n) { dns_log("sendto %s errno=%d", server, errno); break; }
        unsigned char r[1500];
        int rn = (int)recvfrom(fd, r, sizeof r, 0, nullptr, nullptr);
        if (rn < 12) { dns_log("recv %s rn=%d errno=%d", server, rn, errno); continue; }
        if (r[0] != q[0] || r[1] != q[1] || (r[3] & 0x0F) != 0) { dns_log("bad reply from %s rcode=%d", server, r[3] & 0x0F); continue; }
        int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
        int off = 12;
        for (int i = 0; i < qd && off < rn; i++) {            // skip questions
            while (off < rn && r[off]) { if ((r[off] & 0xC0) == 0xC0) { off++; break; } off += r[off] + 1; }
            off += 5;
        }
        for (int i = 0; i < an && off + 10 < rn; i++) {        // answers (CNAMEs are skipped)
            if ((r[off] & 0xC0) == 0xC0) off += 2;
            else { while (off < rn && r[off]) off += r[off] + 1; off++; }
            if (off + 10 > rn) break;
            int type = (r[off] << 8) | r[off + 1];
            int rdlen = (r[off + 8] << 8) | r[off + 9];
            off += 10;
            if (type == 1 && rdlen == 4 && off + 4 <= rn) { memcpy(&out->s_addr, r + off, 4); result = 0; break; }
            off += rdlen;
        }
    }
    close(fd);
    return result;
}

static bool parse_ipv4(const char *s, struct in_addr *out) {
    unsigned v[4]; int n = 0; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4) return false;
    for (n = 0; n < 4; n++) if (v[n] > 255) return false;
    unsigned char *b = (unsigned char *)&out->s_addr;
    b[0] = v[0]; b[1] = v[1]; b[2] = v[2]; b[3] = v[3];
    return true;
}

void dns_log(const char *fmt, ...) {
    FILE *f = fopen("/data/QuranPS5/network.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = nullptr;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) return EAI_FAMILY;

    struct in_addr addr; memset(&addr, 0, sizeof addr);
    if (!node) {
        addr.s_addr = (hints && (hints->ai_flags & AI_PASSIVE)) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    } else if (!parse_ipv4(node, &addr)) {
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
        if (!strcmp(node, "localhost")) addr.s_addr = htonl(INADDR_LOOPBACK);
        else {
            std::lock_guard<std::mutex> lock(g_net_mutex);
            bool found = false;
            for (auto &c : g_dns_cache)
                if (c.host[0] && !strcmp(c.host, node)) { addr = c.addr; found = true; break; }
            static const char *servers[] = { "1.1.1.1", "8.8.8.8", "9.9.9.9", "1.0.0.1" };
            for (int i = 0; !found && i < 4; i++) {
                if (dns_query(servers[i], node, &addr) == 0) {
                    found = true;
                    for (auto &c : g_dns_cache) if (!c.host[0]) { snprintf(c.host, sizeof c.host, "%s", node); c.addr = addr; break; }
                }
            }
            unsigned char *ip = (unsigned char *)&addr.s_addr;
            if (!found) dns_log("%s -> lookup FAILED", node);
            (void)ip;
            if (!found) return EAI_NONAME;
        }
    }
    int port = service ? atoi(service) : 0;

    struct Block { struct addrinfo ai; struct sockaddr_in sin; };
    Block *b = (Block *)calloc(1, sizeof(Block));
    if (!b) return EAI_MEMORY;
    b->sin.sin_len = sizeof(struct sockaddr_in);
    b->sin.sin_family = AF_INET;
    b->sin.sin_port = htons((unsigned short)port);
    b->sin.sin_addr = addr;
    b->ai.ai_family = AF_INET;
    b->ai.ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
    b->ai.ai_protocol = (hints && hints->ai_protocol) ? hints->ai_protocol
                      : (b->ai.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    b->ai.ai_addrlen = sizeof(struct sockaddr_in);
    b->ai.ai_addr = (struct sockaddr *)&b->sin;
    *res = &b->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai) {
    while (ai) { struct addrinfo *next = ai->ai_next; free(ai); ai = next; }
}

// Shell-style pattern matching (only '*', '?' and '[...]'), also from the WebKit library otherwise
static int fnm(const char *p, const char *s) {
    for (;; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 0;
            for (; *s; s++) if (!fnm(p, s)) return 0;
            return 1;
        }
        if (!*s) return *p ? 1 : 0;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1; int neg = (*q == '!' || *q == '^'); if (neg) q++;
            int ok = 0;
            for (; *q && *q != ']'; q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if (*s >= *q && *s <= q[2]) ok = 1; q += 2; }
                else if (*q == *s) ok = 1;
            }
            if (!*q || ok == neg) return 1;
            p = q; continue;
        }
        if (*p != *s) return 1;
    }
}
int fnmatch(const char *pattern, const char *string, int) { return fnm(pattern, string) ? 1 : 0; }


// ---------- libc functions curl/OpenSSL use that the clean-room title libc does not export ----------
char *basename(char *path) {
    static char dot[] = ".";
    if (!path || !*path) return dot;
    char *end = path + strlen(path) - 1;
    while (end > path && *end == '/') *end-- = 0;
    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) if (*--p == (unsigned char)c) return (void *)p;
    return nullptr;
}
char *realpath(const char *path, char *resolved) {
    if (!path) { errno = EINVAL; return nullptr; }
    char *out = resolved ? resolved : (char *)malloc(1024);
    if (!out) return nullptr;
    snprintf(out, 1024, "%s", path);
    return out;
}
int gethostname(char *name, size_t len) {
    if (!name || !len) return -1;
    snprintf(name, len, "ps5");
    return 0;
}
struct ifaddrs;
int getifaddrs(struct ifaddrs **ifap) { if (ifap) *ifap = nullptr; errno = ENOSYS; return -1; }
void freeifaddrs(struct ifaddrs *) {}
void makecontext(void *, void (*)(), int, ...) {}

// local strdup: avoids an absolute (R_X86_64_64) relocation against an imported function
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

// connect wrapper: connect in blocking mode (restoring the socket's flags afterwards) and log the outcome.
int __real_connect(int fd, const struct sockaddr *addr, socklen_t len);
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    int fl = fcntl(fd, F_GETFL);
    bool nb = fl >= 0 && (fl & O_NONBLOCK);
    if (nb) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    errno = 0;
    int rc = __real_connect(fd, addr, len);
    int e = errno;
    if (nb) fcntl(fd, F_SETFL, fl);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    const unsigned char *ip = (const unsigned char *)&sin->sin_addr.s_addr;
    if (rc != 0) dns_log("connect fd=%d fam=%d len=%u sinlen=%u %u.%u.%u.%u:%u nb=%d -> rc=%d errno=%d",
            fd, addr ? addr->sa_family : -1, (unsigned)len, addr ? (unsigned)((const unsigned char *)addr)[0] : 0,
            ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(sin->sin_port), nb, rc, e);
    errno = e;
    return rc;
}

// fcntl wrapper: the PS5 kernel rejects F_SETFD (close-on-exec) with EINVAL, and curl treats that as fatal.
// Nothing is ever exec'd from a title, so the flag has no meaning here: report success.
int __real_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (cmd == F_SETFD) return 0;
    return __real_fcntl(fd, cmd, arg);
}

}

/*
 * cyc_tcp.c - the cycle host's TCP debug server (cyc_tcp.h).
 */
#include "cyc_tcp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
static bool would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void nonblock(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static void nonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

#define LINE_MAX_BYTES 8192
#define MAX_COMMANDS 128

typedef struct {
    const char *name, *summary;
    CycTcpHandler handler;
} Command;

static sock_t s_listen = BAD_SOCK, s_client = BAD_SOCK;
static int s_port;
static char s_buf[LINE_MAX_BYTES];
static size_t s_len;
static Command s_cmds[MAX_COMMANDS];
static int s_ncmds;

static void send_all(const char *data, size_t n)
{
    while (s_client != BAD_SOCK && n) {
        int w = (int)send(s_client, data, (int)n, 0);
        if (w > 0) { data += w; n -= (size_t)w; continue; }
        if (w < 0 && would_block()) continue;      /* small replies: spin until the kernel takes them */
        close_sock(s_client);
        s_client = BAD_SOCK;
    }
}

static void send_line(const char *text)
{
    send_all(text, strlen(text));
    send_all("\n", 1);
}

void cyc_tcp_ok(int id, const char *fields)
{
    size_t n = (fields ? strlen(fields) : 0) + 64;
    char *b = (char *)malloc(n);
    if (!b) return;
    snprintf(b, n, "{\"id\":%d,\"ok\":true%s%s}", id, fields && *fields ? "," : "", fields ? fields : "");
    send_line(b);
    free(b);
}

void cyc_tcp_quote(const char *s, char *out, size_t n)
{
    size_t at = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[at++] = '"';
    for (; s && *s && at + 7 < n; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[at++] = '\\'; out[at++] = (char)c; }
        else if (c == '\n') { out[at++] = '\\'; out[at++] = 'n'; }
        else if (c < 0x20) at += (size_t)snprintf(out + at, n - at, "\\u%04x", c);
        else out[at++] = (char)c;
    }
    out[at++] = '"';
    out[at] = 0;
}

void cyc_tcp_err(int id, const char *message)
{
    char q[512], b[600];
    cyc_tcp_quote(message, q, sizeof(q));
    snprintf(b, sizeof(b), "{\"id\":%d,\"ok\":false,\"err\":%s}", id, q);
    send_line(b);
}

/* ---- tiny JSON field reader: "key": "value" | number | true/false ---- */

static const char *find_key(const char *line, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = line; (p = strchr(p, '"')); ++p) {
        if (!strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
            const char *q = p + 2 + kl;
            while (*q == ' ' || *q == '\t') ++q;
            if (*q != ':') continue;
            ++q;
            while (*q == ' ' || *q == '\t') ++q;
            return q;
        }
    }
    return NULL;
}

bool cyc_tcp_str(const char *line, const char *key, char *out, size_t n)
{
    const char *v = find_key(line, key);
    if (!v || *v != '"' || !n) return false;
    size_t at = 0;
    for (++v; *v && *v != '"' && at + 1 < n; ++v) {
        if (*v == '\\' && v[1]) {
            ++v;
            out[at++] = *v == 'n' ? '\n' : *v;
        } else {
            out[at++] = *v;
        }
    }
    out[at] = 0;
    return true;
}

bool cyc_tcp_long(const char *line, const char *key, long *out)
{
    const char *v = find_key(line, key);
    if (!v) return false;
    if (*v == '"') ++v;
    char *end;
    long x = strtol(v, &end, 0);
    if (end == v) return false;
    *out = x;
    return true;
}

bool cyc_tcp_bool(const char *line, const char *key, bool *out)
{
    const char *v = find_key(line, key);
    if (!v) return false;
    if (!strncmp(v, "true", 4) || *v == '1') *out = true;
    else if (!strncmp(v, "false", 5) || *v == '0') *out = false;
    else return false;
    return true;
}

/* ---- the server ---- */

static void help(int id, const char *line)
{
    (void)line;
    size_t cap = 256;
    for (int i = 0; i < s_ncmds; ++i) cap += strlen(s_cmds[i].name) + strlen(s_cmds[i].summary) * 2 + 40;
    char *b = (char *)malloc(cap), q[512];
    if (!b) return;
    size_t at = (size_t)snprintf(b, cap, "\"commands\":[");
    for (int i = 0; i < s_ncmds; ++i) {
        cyc_tcp_quote(s_cmds[i].summary, q, sizeof(q));
        at += (size_t)snprintf(b + at, cap - at, "%s{\"name\":\"%s\",\"summary\":%s}", i ? "," : "", s_cmds[i].name, q);
    }
    snprintf(b + at, cap - at, "]");
    cyc_tcp_ok(id, b);
    free(b);
}

void cyc_tcp_register(const char *name, const char *summary, CycTcpHandler handler)
{
    if (s_ncmds < MAX_COMMANDS) s_cmds[s_ncmds++] = (Command){ name, summary ? summary : "", handler };
    else fprintf(stderr, "[cyc tcp] command table full: %s is not served\n", name);
}

bool cyc_tcp_start(int port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
#endif
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == BAD_SOCK) return false;
    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_listen, (struct sockaddr *)&a, sizeof(a)) || listen(s_listen, 1)) {
        close_sock(s_listen);
        s_listen = BAD_SOCK;
        return false;
    }
    nonblock(s_listen);
    s_port = port;
    bool have_help = false;
    for (int i = 0; i < s_ncmds; ++i) have_help |= !strcmp(s_cmds[i].name, "help");
    if (!have_help) cyc_tcp_register("help", "list the commands", help);
    return true;
}

int cyc_tcp_port(void) { return s_listen != BAD_SOCK ? s_port : 0; }

static void dispatch(char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (!n) return;
    char cmd[64];
    long id = 0;
    if (!cyc_tcp_str(line, "cmd", cmd, sizeof(cmd))) snprintf(cmd, sizeof(cmd), "%s", line);
    cyc_tcp_long(line, "id", &id);
    for (int i = 0; i < s_ncmds; ++i)
        if (!strcmp(cmd, s_cmds[i].name)) { s_cmds[i].handler((int)id, line); return; }
    cyc_tcp_err((int)id, "unknown command (try help)");
}

void cyc_tcp_poll(void)
{
    if (s_listen == BAD_SOCK) return;
    if (s_client == BAD_SOCK) {
        sock_t c = accept(s_listen, NULL, NULL);
        if (c == BAD_SOCK) return;
        nonblock(c);
        s_client = c;
        s_len = 0;
    }
    for (;;) {
        char tmp[2048];
        int r = (int)recv(s_client, tmp, sizeof(tmp), 0);
        if (r == 0 || (r < 0 && !would_block())) {
            close_sock(s_client);
            s_client = BAD_SOCK;
            return;
        }
        if (r < 0) return;
        for (int i = 0; i < r; ++i) {
            if (tmp[i] == '\n') {
                s_buf[s_len] = 0;
                s_len = 0;
                dispatch(s_buf);
                if (s_client == BAD_SOCK) return;
            } else if (s_len + 1 < sizeof(s_buf)) {
                s_buf[s_len++] = tmp[i];
            }
        }
    }
}

void cyc_tcp_stop(void)
{
    if (s_client != BAD_SOCK) close_sock(s_client);
    if (s_listen != BAD_SOCK) close_sock(s_listen);
    s_client = s_listen = BAD_SOCK;
    s_port = 0;
#ifdef _WIN32
    WSACleanup();
#endif
}

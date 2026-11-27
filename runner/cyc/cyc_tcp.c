#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/*
 * cyc_tcp.c - the cycle host's TCP debug server (cyc_tcp.h).
 */
#include "cyc_tcp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics_server.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define LINE_MAX_BYTES 8192
#define MAX_COMMANDS 128

typedef struct {
    const char *name, *summary;
    CycTcpHandler handler;
} Command;

static DiagServer *s_server;
static bool s_stopping;
static Command s_cmds[MAX_COMMANDS];
static int s_ncmds;

static void send_line(const char *text)
{
    if (s_server) (void)diag_server_reply(s_server, text);
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

static void dispatch(char *line);
static void receive_line(void *user, DiagServer *server, const char *arguments)
{
    (void)user; (void)server;
    if (s_stopping) return;
    char line[LINE_MAX_BYTES];
    size_t n = strlen(arguments);
    if (n >= sizeof(line)) { cyc_tcp_err(0, "command line is too long"); return; }
    memcpy(line, arguments, n + 1);
    dispatch(line);
}
static void framing_error(void *user, DiagServer *server, const char *reason)
{
    (void)user; (void)server;
    if (!s_stopping) cyc_tcp_err(0, reason);
}
bool cyc_tcp_start(int port)
{
    s_stopping = false;
    s_server = diag_server_create((uint16_t)port);
    if (!s_server) return false;
    if (!diag_server_protocol(s_server, LINE_MAX_BYTES - 1, 64u * 1024u * 1024u, framing_error, NULL) ||
        !diag_server_raw_handler(s_server, receive_line, NULL)) {
        diag_server_destroy(s_server); s_server = NULL; return false;
    }
    bool have_help = false;
    for (int i = 0; i < s_ncmds; ++i) have_help |= !strcmp(s_cmds[i].name, "help");
    if (!have_help) cyc_tcp_register("help", "list the commands", help);
    return true;
}
int cyc_tcp_port(void) { return s_server ? diag_server_port(s_server) : 0; }

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
    if (s_server) (void)diag_server_poll(s_server);
}

void cyc_tcp_stop(void)
{
    if (!s_server) return;
    s_stopping = true;
    while (diag_server_pending_output(s_server)) {
        (void)diag_server_poll(s_server);
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec delay = { 0, 1000000 };
        nanosleep(&delay, NULL);
#endif
    }
    diag_server_destroy(s_server);
    s_server = NULL;
}

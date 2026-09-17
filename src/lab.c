#include "lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

/* ===================== Layer 1: pure helpers ===================== */

int parse_reply_code(const char *line) {
    if (!line || line[0] < '0' || line[0] > '9' ||
        line[1] < '0' || line[1] > '9' ||
        line[2] < '0' || line[2] > '9') return -1;
    if (line[3] != ' ' && line[3] != '-') return -1;   /* must be space or hyphen */
    return (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
}

/* Only the space-form is final. The code is on line 1, but you still have to
 * consume every hyphen-form continuation line or the stream desyncs. */
int reply_is_final(const char *line) { return (line && line[3] == ' '); }

char *cmd_helo(const char *host) {
    size_t n = strlen(host);
    char *out = malloc(n + sizeof "HELO \r\n");
    if (out) snprintf(out, n + sizeof "HELO \r\n", "HELO %s\r\n", host);
    return out;
}
char *cmd_mail_from(const char *from) {
    size_t n = strlen(from);
    char *out = malloc(n + sizeof "MAIL FROM:<>\r\n");
    if (out) snprintf(out, n + sizeof "MAIL FROM:<>\r\n", "MAIL FROM:<%s>\r\n", from);
    return out;
}
char *cmd_rcpt_to(const char *to) {
    size_t n = strlen(to);
    char *out = malloc(n + sizeof "RCPT TO:<>\r\n");
    if (out) snprintf(out, n + sizeof "RCPT TO:<>\r\n", "RCPT TO:<%s>\r\n", to);
    return out;
}
const char *cmd_data(void) { return "DATA\r\n"; }
const char *cmd_quit(void) { return "QUIT\r\n"; }

/* CRLF injection guard: reject any field containing a bare CR or LF.
 * This is the SMTP version of the metacharacter-escaping rule — CRLF is the
 * framing metacharacter, and letting one through lets input inject a new line. */
int has_bare_crlf(const char *s) {
    for (; s && *s; ++s) if (*s == '\r' || *s == '\n') return 1;
    return 0;
}

/* Dot-stuff + CRLF normalization, single allocation (leak-safe).
 * Worst-case output is <= 3*len+8 (a '.' added per line + '\n'->"\r\n"), so the
 * fixed cap below is provably sufficient — no realloc to leak. */
char *dot_stuff(const char *body, size_t len) {
    char *out = malloc(len*3 + 8);
    if (!out) return NULL;
    size_t o = 0;
    const char *p = body, *end = body + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t ll = (size_t)(line_end - p);
        if (ll > 0 && line_end[-1] == '\r') ll--;      /* strip CR from CRLF input */
        if (ll > 0 && p[0] == '.') out[o++] = '.';     /* escape the sentinel byte */
        memcpy(out + o, p, ll); o += ll;
        out[o++] = '\r'; out[o++] = '\n';              /* every line CRLF-terminated */
        p = nl ? nl + 1 : end;
    }
    out[o] = '\0';
    return out;
}

/* Assembles exactly: headers, blank line, dot-stuffed body, lone '.' terminator.
 * stuffed already ends in "\r\n" (or is empty), so we append just ".\r\n". */
char *build_data_payload(const char *from, const char *to, const char *subject, const char *stuffed) {
    size_t need = strlen(from)+strlen(to)+strlen(subject?subject:"")+
                  strlen(stuffed?stuffed:"") + sizeof("From: \r\nTo: \r\nSubject: \r\n\r\n.\r\n");
    char *out = malloc(need);
    if (out) snprintf(out, need, "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n%s.\r\n",
                      from, to, subject?subject:"", stuffed?stuffed:"");
    return out;
}

/* ===================== Layer 3: socket transport ===================== */

typedef struct { int fd; char buf[8192]; size_t pos, end; } sock_ctx_t;

static int sock_read_line(void *ctx, char *buf, size_t bufsz) {
    sock_ctx_t *s = ctx;
    for (;;) {
        /* 1. Look for a complete line ALREADY in the buffer before touching the wire. */
        size_t i = s->pos;
        while (i < s->end && s->buf[i] != '\n') ++i;
        if (i < s->end) {
            size_t len = i - s->pos;
            if (len > 0 && s->buf[s->pos + len - 1] == '\r') --len;   /* strip CRLF */
            if (len >= bufsz) return -1;                              /* line too long */
            memcpy(buf, s->buf + s->pos, len); buf[len] = '\0';
            s->pos = i + 1;                                           /* advance past \n */
            if (s->pos >= sizeof s->buf / 2) {                         /* compaction insurance */
                size_t rem = s->end - s->pos;
                memmove(s->buf, s->buf + s->pos, rem);
                s->pos = 0; s->end = rem;
            }
            return (int)len;
        }
        /* 2. No complete line: compact, then refill ONLY the free space. */
        size_t rem = s->end - s->pos;
        memmove(s->buf, s->buf + s->pos, rem);
        s->pos = 0; s->end = rem;
        if (rem >= sizeof s->buf) return -1;                           /* line exceeds buffer */
        ssize_t n = recv(s->fd, s->buf + s->end, sizeof s->buf - s->end, 0);
        if (n <= 0) return -1;                                        /* EOF or error mid-reply */
        s->end += (size_t)n;
    }
}

static int sock_write_all(void *ctx, const char *data, size_t len) {
    sock_ctx_t *s = ctx;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(s->fd, data + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int sock_connect(const char *host, const char *port, transport_t *t) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;          /* don't assume dotted quad */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    sock_ctx_t *ctx = calloc(1, sizeof *ctx);
    if (!ctx) { close(fd); return -1; }
    ctx->fd = fd;
    t->read_line = sock_read_line;
    t->write_all = sock_write_all;
    t->ctx = ctx;
    return 0;
}

void sock_close(transport_t *t) {
    if (!t || !t->ctx) return;
    sock_ctx_t *s = t->ctx;
    if (s->fd >= 0) close(s->fd);
    free(s);
    t->ctx = NULL; t->read_line = NULL; t->write_all = NULL;
}

/* ===================== Layer 2: session primitives ===================== */

/* Read a WHOLE reply (looping continuation lines), return the final code. */
int read_reply(transport_t *t, char *detail, size_t detail_sz) {
    int code = -1;
    for (;;) {
        char line[512];
        if (t->read_line(t->ctx, line, sizeof line) < 0) return -1;  /* hung up */
        int c = parse_reply_code(line);
        if (c < 0) return -1;                                       /* malformed */
        code = c;
        if (detail_sz) { strncpy(detail, line, detail_sz-1); detail[detail_sz-1] = 0; }
        if (reply_is_final(line)) break;                            /* space-form: done */
    }
    return code;
}

/* Write one command line, read the full reply, compare to expected. */
int send_command(transport_t *t, const char *cmd, int expected) {
    if (t->write_all(t->ctx, cmd, strlen(cmd)) != 0) return -1;
    char detail[512];
    int got = read_reply(t, detail, sizeof detail);
    if (got < 0) { fprintf(stderr, "server hung up after: %s\n", cmd); return -1; }
    if (got != expected) {
        fprintf(stderr, "expected %d but server sent: %s\n", expected, detail);
        return -1;
    }
    return 0;
}

/* ===================== SESSION ===================== */
int run_session(transport_t *t, const mail_cfg_t *cfg)
{
    (void)t;
    (void)cfg;

//# STEP 1 — greeting: READ ONLY. No command is sent before this.
    char detail[512];
    int code = read_reply(t, detail, sizeof detail);
    if (code < 0) {printf("server hung up during greeting\n");  return -1;}
    if (code != 220) {fprintf(stderr, "expected 220 but server sent: %s\n", detail); return -1;}

//# STEPS 2–5 — the four commands. Identical shape, so consider a tiny local helper (see below) to keep the free() discipline automatic.
    char* cmd = cmd_helo(cfg->helo_host);
    if (send_command(t, cmd, 250) != 0) { free(cmd); return -1;}
    free(cmd);

    cmd = cmd_mail_from(cfg->from);
    if (send_command(t, cmd, 250) != 0) { free(cmd); return -1;}
    free(cmd);

    cmd = cmd_rcpt_to(cfg->to);
    if (send_command(t, cmd, 250) != 0)  { free(cmd); return -1;}
    free(cmd);

    // DATA expects 354 — a mode change ("now send bytes"), not completion.
    if (send_command(t, "DATA\r\n", 354) != 0) return -1;   // cmd_data()

//# STEP 6 — the payload. NOT send_command: this is multi-line data ending in a sentinel, not a command line. Write it raw, then check the reply.
    char *stuffed = dot_stuff(cfg->body, strlen(cfg->body));
    if (!stuffed) return -1; // failed malloc
    char *payload = build_data_payload(cfg->from, cfg->to, cfg->subject, stuffed);
    free(stuffed); // free immediately, done with it
    if (!payload) return -1;
    if (t->write_all(t->ctx, payload, strlen(payload)) != 0) { free(payload); fprintf(stderr, "send failed during DATA\n"); return -1; }
    free(payload);
    code = read_reply(t, detail, sizeof detail);  // the "queued" ack
    if (code < 0) { printf("server hung up after DATA\n"); return -1; }
    if (code != 250) { fprintf(stderr, "expected 220 but server sent: %s\n", detail); return -1; }

//# STEP 7 — QUIT. Even though the message was queued, a bad final code
    // # still fails the session (exit 2 territory).
    if (send_command(t, cmd_quit(), 221) != 0) return -1;
    return 0;
}
/* lab-test.c — Unity test suite for the P1 SMTP client.
 *
 * Replaces the starter file wholesale (the get_greeting placeholder tests a
 * function that does not exist in src/lab.h).
 *
 * Structure:
 *   1. Fake transport — a test double implementing the same read_line/
 *      write_all contract as the socket layer, but driven by a string.
 *   2. Layer 1 tests — every pure protocol helper.
 *   3. Layer 2 tests — read_reply / send_command against the fake, then
 *      run_session: happy path, multi-line replies, byte-at-a-time delivery,
 *      dot stuffing, hangup, and EACH wrong status code in the sequence.
 *   4. Layer 3 tests — real sockets via a loopback listener (no network),
 *      plus connect failures and close on an empty ctx.
 *
 * NOTE: the run_session tests are the contract for your implementation.
 * They will FAIL until you fill in run_session in src/lab.c; everything
 * else should pass immediately.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "harness/unity.h"
#include "../src/lab.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* =====================================================================
 * 1. Fake transport (test double for the socket layer)
 * ===================================================================== */

typedef struct {
    const char *script;   /* scripted server output */
    size_t sent;          /* bytes pulled from script so far */
    int chunk;            /* bytes per refill (0 = all remaining at once) */
    char *buf;            /* sliding read buffer, same discipline as sock_ */
    size_t pos, end, cap;
    char *written;        /* log of every byte the client wrote */
    size_t wlen, wcap;
    int write_fail;       /* if set, write_all fails */
} fake_ctx_t;

static void fake_free(fake_ctx_t *f);   /* fwd decl: used by fake_new's error path */

static fake_ctx_t *fake_new(const char *script, size_t cap, int chunk) {
    fake_ctx_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->script = script ? script : "";
    f->cap = cap ? cap : 64;
    f->buf = malloc(f->cap);
    f->chunk = chunk;
    f->written = malloc(1);
    f->wcap = 1;
    if (f->written) f->written[0] = '\0';
    if (!f->buf || !f->written) { fake_free(f); return NULL; }
    return f;
}

static void fake_free(fake_ctx_t *f) {
    if (!f) return;
    free(f->buf);
    free(f->written);
    free(f);
}

/* Contract: return one complete line (CRLF stripped) in out, or -1 on
 * EOF/hangup/line-too-long. Refills from the script only when no complete
 * line is already buffered — the same rule as the real transport. */
static int fake_read_line(void *ctx, char *out, size_t outsz) {
    fake_ctx_t *f = ctx;
    for (;;) {
        /* 1. Look for a complete line ALREADY in the buffer first. */
        size_t i = f->pos;
        while (i < f->end && f->buf[i] != '\n') i++;
        if (i < f->end) {
            size_t len = i - f->pos;
            if (len > 0 && f->buf[f->pos + len - 1] == '\r') len--;
            if (len >= outsz) return -1;          /* caller's buffer too small */
            memcpy(out, f->buf + f->pos, len);
            out[len] = '\0';
            size_t rem = f->end - (i + 1);
            memmove(f->buf, f->buf + i + 1, rem);
            f->pos = 0;
            f->end = rem;
            return (int)len;
        }
        /* 2. No complete line: compact, then pull more from the script. */
        size_t rem = f->end - f->pos;
        memmove(f->buf, f->buf + f->pos, rem);
        f->pos = 0;
        f->end = rem;
        if (rem >= f->cap) return -1;             /* line longer than buffer */
        size_t left = strlen(f->script) - f->sent;
        if (left == 0) return -1;                 /* EOF: hung up mid-line */
        size_t room = f->cap - f->end;            /* model recv(): never pull more than fits */
        size_t take = (f->chunk > 0 && (size_t)f->chunk < left)
                          ? (size_t)f->chunk : left;
        if (take > room) take = room;             /* clamp to free space */
        memcpy(f->buf + f->end, f->script + f->sent, take);
        f->end += take;
        f->sent += take;
    }
}

static int fake_write_all(void *ctx, const char *data, size_t n) {
    fake_ctx_t *f = ctx;
    if (f->write_fail) return -1;
    if (f->wlen + n + 1 > f->wcap) {
        size_t nc = f->wcap;
        while (f->wlen + n + 1 > nc) nc *= 2;
        char *nw = realloc(f->written, nc);
        if (!nw) return -1;
        f->written = nw;
        f->wcap = nc;
    }
    memcpy(f->written + f->wlen, data, n);
    f->wlen += n;
    f->written[f->wlen] = '\0';
    return 0;
}

static transport_t make_transport(fake_ctx_t *f) {
    transport_t t;
    t.read_line = fake_read_line;
    t.write_all = fake_write_all;
    t.ctx = f;
    return t;
}

/* =====================================================================
 * Shared fixtures: the canonical session script and expected transcript
 * ===================================================================== */

#define R_GREET   "220 smtp.example.com ESMTP ready\r\n"
#define R_HELO    "250 smtp.example.com\r\n"
#define R_MAIL    "250 2.1.0 Ok\r\n"
#define R_RCPT    "250 2.1.5 Ok\r\n"
#define R_DATA    "354 End data with .\r\n"
#define R_QUEUED  "250 2.0.0 Ok: queued\r\n"
#define R_BYE     "221 Bye\r\n"

#define SCRIPT_OK (R_GREET R_HELO R_MAIL R_RCPT R_DATA R_QUEUED R_BYE)

#define W_HELO    "HELO onyx.boisestate.edu\r\n"
#define W_MAIL    "MAIL FROM:<me@boisestate.edu>\r\n"
#define W_RCPT    "RCPT TO:<you@example.com>\r\n"
#define W_DATA    "DATA\r\n"
#define W_PAYLOAD "From: me@boisestate.edu\r\n" \
                  "To: you@example.com\r\n" \
                  "Subject: hello\r\n" \
                  "\r\n" \
                  "This is the message body.\r\n" \
                  ".\r\n"
#define W_QUIT    "QUIT\r\n"

#define EXPECT_TRANSCRIPT (W_HELO W_MAIL W_RCPT W_DATA W_PAYLOAD W_QUIT)

static mail_cfg_t std_cfg(void) {
    mail_cfg_t c;
    c.from = "me@boisestate.edu";
    c.to = "you@example.com";
    c.subject = "hello";
    c.body = "This is the message body.\n";  /* LF endings: as from stdin */
    c.helo_host = "onyx.boisestate.edu";
    return c;
}

void setUp(void) {}
void tearDown(void) {}

/* =====================================================================
 * 2. Layer 1 — pure protocol helpers
 * ===================================================================== */

void test_parse_reply_code(void) {
    TEST_ASSERT_EQUAL_INT(220, parse_reply_code("220 smtp.example.com ESMTP ready"));
    TEST_ASSERT_EQUAL_INT(250, parse_reply_code("250-PIPELINING")); /* hyphen form parses too */
    TEST_ASSERT_EQUAL_INT(354, parse_reply_code("354 End data with ."));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("25 Ok"));           /* only two digits */
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("250x Ok"));         /* no space/hyphen after code */
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code("abc"));
    TEST_ASSERT_EQUAL_INT(-1, parse_reply_code(NULL));
}

void test_reply_is_final(void) {
    TEST_ASSERT_EQUAL_INT(1, reply_is_final("250 Ok"));
    TEST_ASSERT_EQUAL_INT(0, reply_is_final("250-PIPELINING"));     /* hyphen = more coming */
    TEST_ASSERT_EQUAL_INT(0, reply_is_final("25 x"));
    TEST_ASSERT_EQUAL_INT(0, reply_is_final(NULL));
}

void test_cmd_builders(void) {
    char *s;
    s = cmd_helo("onyx.boisestate.edu");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("HELO onyx.boisestate.edu\r\n", s);
    free(s);
    s = cmd_mail_from("me@boisestate.edu");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("MAIL FROM:<me@boisestate.edu>\r\n", s);
    free(s);
    s = cmd_rcpt_to("you@example.com");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("RCPT TO:<you@example.com>\r\n", s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("DATA\r\n", cmd_data());
    TEST_ASSERT_EQUAL_STRING("QUIT\r\n", cmd_quit());
}

void test_dot_stuff_plain(void) {
    const char *body = "line one\nline two\n";
    char *s = dot_stuff(body, strlen(body));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("line one\r\nline two\r\n", s);
    free(s);
}

void test_dot_stuff_escapes_leading_dots(void) {
    /* A '.' in column 0 is the DATA sentinel — it must be doubled. */
    const char *body = "a\n.b\n..c\n";
    char *s = dot_stuff(body, strlen(body));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("a\r\n..b\r\n...c\r\n", s);
    free(s);
}

void test_dot_stuff_empty_and_lone_dot(void) {
    char *s = dot_stuff("", 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("", s);
    free(s);
    /* A body that is exactly "." must survive as content, not end the data. */
    s = dot_stuff(".", 1);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("..\r\n", s);
    free(s);
}

void test_dot_stuff_normalizes_endings(void) {
    /* Mixed CRLF/LF, no trailing newline: every line out is CRLF-terminated. */
    const char *body = "a\r\nb\nc";
    char *s = dot_stuff(body, strlen(body));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("a\r\nb\r\nc\r\n", s);
    free(s);
}

void test_build_data_payload(void) {
    const char *body = "This is the message body.\n";
    char *stuffed = dot_stuff(body, strlen(body));
    TEST_ASSERT_NOT_NULL(stuffed);
    char *p = build_data_payload("me@boisestate.edu", "you@example.com",
                                 "hello", stuffed);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING(
        "From: me@boisestate.edu\r\n"
        "To: you@example.com\r\n"
        "Subject: hello\r\n"
        "\r\n"
        "This is the message body.\r\n"
        ".\r\n", p);
    free(p);
    free(stuffed);
}

void test_build_data_payload_nulls(void) {
    /* NULL subject and empty stuffed body: headers, blank line, lone dot. */
    char *p = build_data_payload("a@b.c", "d@e.f", NULL, "");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING(
        "From: a@b.c\r\n"
        "To: d@e.f\r\n"
        "Subject: \r\n"
        "\r\n"
        ".\r\n", p);
    free(p);
}

void test_has_bare_crlf(void) {
    TEST_ASSERT_EQUAL_INT(0, has_bare_crlf("plain subject"));
    TEST_ASSERT_EQUAL_INT(1, has_bare_crlf("a\nb"));
    TEST_ASSERT_EQUAL_INT(1, has_bare_crlf("a\rb"));
    /* the injection case: CRLF in a subject would forge a new header line */
    TEST_ASSERT_EQUAL_INT(1, has_bare_crlf("evil\r\nSubject: forged"));
    TEST_ASSERT_EQUAL_INT(0, has_bare_crlf(NULL));
}

/* =====================================================================
 * 3a. Layer 2 — read_reply
 * ===================================================================== */

void test_read_reply_single_line(void) {
    fake_ctx_t *f = fake_new("250 2.1.0 Ok\r\n", 64, 0);
    transport_t t = make_transport(f);
    char detail[128];
    TEST_ASSERT_EQUAL_INT(250, read_reply(&t, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("250 2.1.0 Ok", detail);
    /* a zero-size detail buffer must be tolerated (no crash) */
    TEST_ASSERT_EQUAL_INT(-1, read_reply(&t, NULL, 0));   /* script exhausted */
    fake_free(f);
}

void test_read_reply_consumes_continuation_lines(void) {
    /* The hyphen lines MUST be consumed: the next read gets the NEXT reply,
     * not a stray continuation line. */
    fake_ctx_t *f = fake_new(
        "250-smtp.example.com\r\n"
        "250-PIPELINING\r\n"
        "250 SIZE 10240000\r\n"
        "221 Bye\r\n", 256, 0);
    transport_t t = make_transport(f);
    char detail[128];
    TEST_ASSERT_EQUAL_INT(250, read_reply(&t, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("250 SIZE 10240000", detail); /* final line, not first */
    TEST_ASSERT_EQUAL_INT(221, read_reply(&t, detail, sizeof detail));
    fake_free(f);
}

void test_read_reply_byte_at_a_time(void) {
    /* chunk=1: every refill delivers a single byte; cap=64 forces many
     * refills and compactions per line. */
    fake_ctx_t *f = fake_new(SCRIPT_OK, 64, 1);
    transport_t t = make_transport(f);
    char detail[128];
    TEST_ASSERT_EQUAL_INT(220, read_reply(&t, detail, sizeof detail));
    TEST_ASSERT_EQUAL_INT(250, read_reply(&t, detail, sizeof detail));
    fake_free(f);
}

void test_read_reply_two_replies_in_one_chunk(void) {
    /* chunk=0 delivers the whole script in one pull: the reader must keep
     * the second reply buffered for the next call. */
    fake_ctx_t *f = fake_new("250 a\r\n221 b\r\n", 64, 0);
    transport_t t = make_transport(f);
    char detail[64];
    TEST_ASSERT_EQUAL_INT(250, read_reply(&t, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("250 a", detail);
    TEST_ASSERT_EQUAL_INT(221, read_reply(&t, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("221 b", detail);
    fake_free(f);
}

void test_read_reply_hangup_mid_line(void) {
    /* Truncated script = server closed before the line completed. */
    fake_ctx_t *f = fake_new("250-abc", 64, 0);
    transport_t t = make_transport(f);
    char detail[64];
    TEST_ASSERT_EQUAL_INT(-1, read_reply(&t, detail, sizeof detail));
    fake_free(f);
}

void test_read_reply_malformed_line(void) {
    fake_ctx_t *f = fake_new("hello world\r\n", 64, 0);
    transport_t t = make_transport(f);
    char detail[64];
    TEST_ASSERT_EQUAL_INT(-1, read_reply(&t, detail, sizeof detail));
    fake_free(f);
}

void test_read_reply_line_longer_than_buffer(void) {
    char filler[201];
    memset(filler, 'x', 200);
    filler[200] = '\0';
    char script[300];
    snprintf(script, sizeof script, "250 %s\r\n", filler); /* 206-byte line */
    fake_ctx_t *f = fake_new(script, 64, 0);               /* buffer holds 64 */
    transport_t t = make_transport(f);
    char detail[512];
    TEST_ASSERT_EQUAL_INT(-1, read_reply(&t, detail, sizeof detail));
    fake_free(f);
}

/* =====================================================================
 * 3b. Layer 2 — send_command
 * ===================================================================== */

void test_send_command_ok(void) {
    fake_ctx_t *f = fake_new("250 ok\r\n", 64, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(0, send_command(&t, "HELO x\r\n", 250));
    TEST_ASSERT_EQUAL_STRING("HELO x\r\n", f->written); /* exactly what was sent */
    fake_free(f);
}

void test_send_command_wrong_code(void) {
    fake_ctx_t *f = fake_new("550 user unknown\r\n", 64, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, send_command(&t, "RCPT TO:<x>\r\n", 250));
    /* the command WAS sent; the failure is in the reply */
    TEST_ASSERT_EQUAL_STRING("RCPT TO:<x>\r\n", f->written);
    fake_free(f);
}

void test_send_command_hangup_after_send(void) {
    fake_ctx_t *f = fake_new("", 64, 0); /* server says nothing at all */
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, send_command(&t, "QUIT\r\n", 221));
    fake_free(f);
}

void test_send_command_write_failure(void) {
    fake_ctx_t *f = fake_new("250 ok\r\n", 64, 0);
    f->write_fail = 1;
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, send_command(&t, "HELO x\r\n", 250));
    TEST_ASSERT_EQUAL_INT(0, (int)f->sent); /* never even read a reply */
    fake_free(f);
}

/* =====================================================================
 * 3c. Layer 2 — run_session: the contract for your implementation
 *     (all of these fail until you write run_session)
 * ===================================================================== */

void test_session_happy_path(void) {
    mail_cfg_t c = std_cfg();   /* local: &std_cfg() is illegal (rvalue) in C */
    fake_ctx_t *f = fake_new(SCRIPT_OK, 4096, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(0, run_session(&t, &c));
    /* one assertion checks order + CRLF + content of the whole session */
    TEST_ASSERT_EQUAL_STRING(EXPECT_TRANSCRIPT, f->written);
    fake_free(f);
}

void test_session_multi_line_greeting(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(
        "220-smtp.example.com\r\n"
        "220-ESMTP\r\n"
        "220 ready\r\n"
        R_HELO R_MAIL R_RCPT R_DATA R_QUEUED R_BYE, 4096, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(0, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(EXPECT_TRANSCRIPT, f->written);
    fake_free(f);
}

void test_session_byte_at_a_time(void) {
    /* the entire session survives 1-byte-at-a-time delivery */
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(SCRIPT_OK, 64, 1);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(0, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(EXPECT_TRANSCRIPT, f->written);
    fake_free(f);
}

void test_session_dot_stuffing(void) {
    mail_cfg_t c = std_cfg();
    c.body = "one\ntwo\n.three\n"; /* a line that starts with the sentinel */
    fake_ctx_t *f = fake_new(SCRIPT_OK, 4096, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(0, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(
        W_HELO W_MAIL W_RCPT W_DATA
        "From: me@boisestate.edu\r\n"
        "To: you@example.com\r\n"
        "Subject: hello\r\n"
        "\r\n"
        "one\r\n"
        "two\r\n"
        "..three\r\n"   /* dot-doubled, not treated as end-of-data */
        ".\r\n"
        W_QUIT, f->written);
    fake_free(f);
}

/* --- each wrong status code in the sequence; client must stop there --- */

void test_session_greeting_wrong(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new("450 try later\r\n" R_HELO, 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING("", f->written); /* nothing sent before greeting */
    fake_free(f);
}

void test_session_helo_wrong(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET "550 helo denied\r\n", 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(W_HELO, f->written); /* stopped here */
    fake_free(f);
}

void test_session_mail_from_wrong(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET R_HELO "550 sender rejected\r\n", 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(W_HELO W_MAIL, f->written);
    fake_free(f);
}

void test_session_rcpt_wrong(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET R_HELO R_MAIL "503 need MAIL first\r\n", 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(W_HELO W_MAIL W_RCPT, f->written);
    fake_free(f);
}

void test_session_data_wrong(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET R_HELO R_MAIL R_RCPT "503 no data allowed\r\n", 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(W_HELO W_MAIL W_RCPT W_DATA, f->written);
    fake_free(f);
}

void test_session_payload_ack_wrong(void) {
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET R_HELO R_MAIL R_RCPT R_DATA
                             "451 request action not taken\r\n", 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(W_HELO W_MAIL W_RCPT W_DATA W_PAYLOAD, f->written);
    fake_free(f);
}

void test_session_quit_wrong(void) {
    /* message was queued, but a bad final code still fails the session */
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET R_HELO R_MAIL R_RCPT R_DATA R_QUEUED
                             "421 service no longer available\r\n", 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(EXPECT_TRANSCRIPT, f->written); /* QUIT was sent */
    fake_free(f);
}

void test_session_hangup_after_data(void) {
    /* server dies after the 354: payload is sent, but no queued/bye comes.
     * No QUIT after an error. */
    mail_cfg_t c = std_cfg();
    fake_ctx_t *f = fake_new(R_GREET R_HELO R_MAIL R_RCPT R_DATA, 256, 0);
    transport_t t = make_transport(f);
    TEST_ASSERT_EQUAL_INT(-1, run_session(&t, &c));
    TEST_ASSERT_EQUAL_STRING(W_HELO W_MAIL W_RCPT W_DATA W_PAYLOAD, f->written);
    fake_free(f);
}

/* =====================================================================
 * 4. Layer 3 — real sockets via a loopback listener (no network needed)
 * ===================================================================== */

static int start_listener(char port[16]) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0; /* let the kernel pick an ephemeral port */
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(lfd); return -1; }
    socklen_t slen = sizeof sa;
    if (getsockname(lfd, (struct sockaddr *)&sa, &slen) != 0) { close(lfd); return -1; }
    snprintf(port, 16, "%u", (unsigned)ntohs(sa.sin_port));
    if (listen(lfd, 1) != 0) { close(lfd); return -1; }
    return lfd;
}

void test_sock_connect_and_read_reply(void) {
    char port[16];
    int lfd = start_listener(port);
    TEST_ASSERT_GREATER_THAN_INT(-1, lfd);

    transport_t t;
    memset(&t, 0, sizeof t);
    TEST_ASSERT_EQUAL_INT(0, sock_connect("127.0.0.1", port, &t));

    int cfd = accept(lfd, NULL, NULL);
    TEST_ASSERT_GREATER_THAN_INT(-1, cfd);
    const char *reply = "220 loopback ESMTP ready\r\n";
    ssize_t wr = write(cfd, reply, strlen(reply));
    TEST_ASSERT_EQUAL_INT((int)strlen(reply), (int)wr);

    /* this drives the REAL recv/sock_read_line over a real socket */
    char detail[128];
    TEST_ASSERT_EQUAL_INT(220, read_reply(&t, detail, sizeof detail));
    TEST_ASSERT_EQUAL_STRING("220 loopback ESMTP ready", detail);

    sock_close(&t);
    close(cfd);
    close(lfd);
}

void test_sock_connect_refused(void) {
    transport_t t;
    memset(&t, 0, sizeof t);
    TEST_ASSERT_EQUAL_INT(-1, sock_connect("127.0.0.1", "1", &t)); /* closed port */
    sock_close(&t); /* must not crash on a never-connected ctx */
}

void test_sock_connect_bad_host(void) {
    transport_t t;
    memset(&t, 0, sizeof t);
    /* .invalid is reserved (RFC 6761): getaddrinfo must fail, fast and offline */
    TEST_ASSERT_EQUAL_INT(-1, sock_connect("no-such-host.invalid", "25", &t));
    sock_close(&t);
}

void test_sock_close_empty_ctx(void) {
    transport_t t;
    memset(&t, 0, sizeof t);
    sock_close(&t);   /* NULL ctx: no-op */
    sock_close(&t);   /* double close: still safe */
}

/* ===================================================================== */

int main(void) {
    UNITY_BEGIN();

    /* Layer 1 — pure helpers */
    RUN_TEST(test_parse_reply_code);
    RUN_TEST(test_reply_is_final);
    RUN_TEST(test_cmd_builders);
    RUN_TEST(test_dot_stuff_plain);
    RUN_TEST(test_dot_stuff_escapes_leading_dots);
    RUN_TEST(test_dot_stuff_empty_and_lone_dot);
    RUN_TEST(test_dot_stuff_normalizes_endings);
    RUN_TEST(test_build_data_payload);
    RUN_TEST(test_build_data_payload_nulls);
    RUN_TEST(test_has_bare_crlf);

    /* Layer 2 — read_reply */
    RUN_TEST(test_read_reply_single_line);
    RUN_TEST(test_read_reply_consumes_continuation_lines);
    RUN_TEST(test_read_reply_byte_at_a_time);
    RUN_TEST(test_read_reply_two_replies_in_one_chunk);
    RUN_TEST(test_read_reply_hangup_mid_line);
    RUN_TEST(test_read_reply_malformed_line);
    RUN_TEST(test_read_reply_line_longer_than_buffer);

    /* Layer 2 — send_command */
    RUN_TEST(test_send_command_ok);
    RUN_TEST(test_send_command_wrong_code);
    RUN_TEST(test_send_command_hangup_after_send);
    RUN_TEST(test_send_command_write_failure);

    /* Layer 2 — run_session (contract: red until you implement it) */
    RUN_TEST(test_session_happy_path);
    RUN_TEST(test_session_multi_line_greeting);
    RUN_TEST(test_session_byte_at_a_time);
    RUN_TEST(test_session_dot_stuffing);
    RUN_TEST(test_session_greeting_wrong);
    RUN_TEST(test_session_helo_wrong);
    RUN_TEST(test_session_mail_from_wrong);
    RUN_TEST(test_session_rcpt_wrong);
    RUN_TEST(test_session_data_wrong);
    RUN_TEST(test_session_payload_ack_wrong);
    RUN_TEST(test_session_quit_wrong);
    RUN_TEST(test_session_hangup_after_data);

    /* Layer 3 — socket transport */
    RUN_TEST(test_sock_connect_and_read_reply);
    RUN_TEST(test_sock_connect_refused);
    RUN_TEST(test_sock_connect_bad_host);
    RUN_TEST(test_sock_close_empty_ctx);

    return UNITY_END();
}
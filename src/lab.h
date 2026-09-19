#ifndef LAB_H
#define LAB_H
#include <stddef.h>

/* ---- Layer 1: pure protocol helpers (no I/O) ---- */
int  parse_reply_code(const char *line);          /* -> 3-digit code, or -1 */
int  reply_is_final(const char *line);            /* 1 if space-form (final) */
char *cmd_helo(const char *host);                 /* "HELO host\r\n"      */
char *cmd_mail_from(const char *from);            /* "MAIL FROM:<f>\r\n"  */
char *cmd_rcpt_to(const char *to);                /* "RCPT TO:<t>\r\n"    */
const char *cmd_data(void);                        /* "DATA\r\n"           */
const char *cmd_quit(void);                        /* "QUIT\r\n"           */
char *dot_stuff(const char *body, size_t len);     /* CRLF + dot-escaped   */
char *build_data_payload(const char *from, const char *to,
                         const char *subject, const char *stuffed);
int  has_bare_crlf(const char *s);                 /* injection guard      */

/* ---- Layer 2: the session, over a swappable transport ---- */
typedef struct transport {
    int  (*read_line)(void *ctx, char *buf, size_t bufsz); /* -> len or -1 */
    int  (*write_all)(void *ctx, const char *data, size_t n); /* 0 ok,-1 err */
    void *ctx;                                       /* the "this" pointer */
} transport_t;

typedef struct {
    const char *from, *to, *subject, *body, *helo_host;
} mail_cfg_t;

int read_reply(transport_t *t, char *detail, size_t detail_sz); /* final code or -1 */
int send_command(transport_t *t, const char *cmd, int expected);/* 0 ok,-1 err */
int run_session(transport_t *t, const mail_cfg_t *cfg);         /* <-- YOURS */

/* ---- Layer 3: socket transport (production backend) ---- */
int  sock_connect(const char *host, const char *port, transport_t *t);
void sock_close(transport_t *t);



#endif // LAB_H

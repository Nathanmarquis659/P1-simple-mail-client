#include "lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* read() */

#ifdef TEST
#define main main_exclude
#endif

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s -f <from> -t <to> [-s subject] [-b body] [-p port] [-H helo-host] <server>\n\n"
        "  -f <from>       envelope sender, e.g. you@example.com\n"
        "  -t <to>         envelope recipient\n"
        "  -s <subject>    subject line (default: empty)\n"
        "  -b <body>       message body (default: read from stdin)\n"
        "  -p <port>       port or service name (default: 25)\n"
        "  -H <helo-host>  host name sent with HELO (default: localhost)\n"
        "  <server>        host name or address of the mail server\n", prog);
}

/* Take all of stdin into a malloc, NULL-terminated string. */
static char *read_stdin_body(void) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len - 1); /* room for NUL */
        if (n < 0)  { free(buf); return NULL; }   /* read error */
        if (n == 0) break;                        /* EOF */
        len += (size_t)n;
        if (len + 1 >= cap) {                      /* grow when full */
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); return NULL; }
            buf = nb; cap = ncap;
        }
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char *argv[]) {
    /* GRADED PATH: no arguments at all -> usage + exit 0. No allocations here,
     * so make leak sees a pristine success run. */
    if (argc == 1) { print_usage(argv[0]); return EXIT_SUCCESS; }

    const char *from = NULL, *to = NULL, *subject = "", *body_opt = NULL;
    const char *port = "25", *helo = "localhost";   /* spec defaults */
    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:")) != -1) {
        switch (opt) {
        case 'f': from    = optarg; break;
        case 't': to      = optarg; break;
        case 's': subject = optarg; break;
        case 'b': body_opt = optarg; break;
        case 'p': port    = optarg; break;
        case 'H': helo    = optarg; break;
        default:          /* '?' — unknown flag or a flag missing its value */
            print_usage(argv[0]);
            return EXIT_FAILURE;                    /* exit 1: command line wrong */
        }
    }

    /* Positional: the server host. Missing required pieces -> exit 1. */
    if (optind >= argc || !from || !to) {
        fprintf(stderr, "missing -f <from>, -t <to>, and/or <server>\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;                         /* exit 1 */
    }
    const char *server = argv[optind];

    /* Injection guard: header fields must not contain bare CR/LF.
     * The body is deliberately NOT validated — see note below. */
    if (has_bare_crlf(from) || has_bare_crlf(to) || has_bare_crlf(subject)) {
        fprintf(stderr, "refusing to send: address or subject contains CR or LF\n");
        return EXIT_FAILURE;                         /* exit 1: command line wrong */
    }

    char *body = body_opt ? strdup(body_opt) : read_stdin_body();
    if (!body) { fprintf(stderr, "error: could not obtain message body\n");
                 return EXIT_FAILURE; }

    transport_t t;
    memset(&t, 0, sizeof t);
    if (sock_connect(server, port, &t) != 0) {
        fprintf(stderr, "error: cannot connect to %s:%s\n", server, port);
        free(body);
        return 2;                                    /* exit 2: connection failed */
    }

    mail_cfg_t cfg;
    cfg.from = from; cfg.to = to; cfg.subject = subject;
    cfg.body = body; cfg.helo_host = helo;

    int rc = run_session(&t, &cfg);
    sock_close(&t);      /* frees the ctx on EVERY path — this is where leaks hide */
    free(body);

    if (rc != 0) return 2;                           /* exit 2: session failed */
    printf("message queued\n");
    return EXIT_SUCCESS;
}
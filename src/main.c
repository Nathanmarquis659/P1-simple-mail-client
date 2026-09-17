#include "lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#ifdef TEST
#define main main_exclude
#endif

/*
 Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]
          [-H helo-host] <server>

  -f <from>       envelope sender, for example you@example.com
  -t <to>         envelope recipient
  -s <subject>    subject line (default: empty)
  -b <body>       message body (default: read from stdin)
  -p <port>       port or service name (default: 25)
  -H <helo-host>  host name sent with HELO (default: localhost)
  <server>        host name or address of the mail server
*/

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -f <from> -t <to> [-s subject] [-b body] [-p port] [-H helo-host] <server>\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -f <from>       envelope sender, for example you@example.com\n");
    fprintf(stderr, "  -t <to>         envelope recipient\n");
    fprintf(stderr, "  -s <subject>    subject line (default: empty)\n");
    fprintf(stderr, "  -b <body>       message body (default: read from stdin)\n");
    fprintf(stderr, "  -p <port>       port or service name (default: 25)\n");
    fprintf(stderr, "  -H <helo-host>  host name sent with HELO (default: localhost)\n");
    fprintf(stderr, "  <server>        host name or address of the mail server\n");
}

int main(int argc, char *argv[]) {
    int opt;

    static struct option long_options[] = {
        {"from",    no_argument,       0, 'f'},
        {"to", no_argument,       0, 't'},
        {"subject", no_argument,       0, 's'},
        {"body", no_argument,       0, 'b'},
        {"port", no_argument,       0, 'p'},
        {"helo-host", no_argument,       0, 'H'},
        {"server", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "hv", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        case 'v':
            break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Process remaining positional arguments
    if (optind >= argc) {
        fprintf(stderr, "Error: Missing required input file.\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *input_file = argv[optind];
    // Core logic goes here...

    return EXIT_SUCCESS;
}
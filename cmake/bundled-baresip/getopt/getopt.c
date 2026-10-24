#include "getopt.h"

#include <stdio.h>
#include <string.h>

char* optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

int getopt(int argc, char* const argv[], const char* optstring) {
    // The next letter of a grouped argument such as -vT.
    static const char* next = NULL;
    const int quiet = optstring[0] == ':';

    if (next == NULL || *next == '\0') {
        if (optind >= argc || argv[optind] == NULL || argv[optind][0] != '-' ||
            argv[optind][1] == '\0') {
            return -1;
        }
        if (strcmp(argv[optind], "--") == 0) {
            ++optind;
            return -1;
        }
        next = argv[optind] + 1;
    }

    const char option = *next++;
    const char* spec = option == ':' ? NULL : strchr(optstring, option);
    if (spec == NULL) {
        optopt = option;
        if (opterr && !quiet) fprintf(stderr, "%s: unknown option -%c\n", argv[0], option);
        if (*next == '\0') ++optind;
        return '?';
    }
    if (spec[1] != ':') {
        if (*next == '\0') ++optind;
        return option;
    }

    if (*next != '\0') {
        optarg = (char*)next;
    } else if (optind + 1 < argc) {
        optarg = argv[++optind];
    } else {
        optopt = option;
        if (opterr && !quiet) {
            fprintf(stderr, "%s: option -%c needs an argument\n", argv[0], option);
        }
        ++optind;
        next = NULL;
        return quiet ? ':' : '?';
    }
    ++optind;
    next = NULL;
    return option;
}

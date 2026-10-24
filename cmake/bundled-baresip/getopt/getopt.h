#pragma once

// POSIX getopt for toolchains without one, such as MSVC. Short options only,
// which is all baresip's main uses.

#ifdef __cplusplus
extern "C" {
#endif

extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char* const argv[], const char* optstring);

#ifdef __cplusplus
}
#endif

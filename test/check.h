#pragma once

#include <cstdio>
#include <cstdlib>

// Unlike assert, CHECK always evaluates its condition, so it is safe to wrap
// calls with side effects and it still runs in Release builds (where NDEBUG
// turns every assert into nothing).
//
// A test that owns resources outside the process, such as child processes,
// can point gCheckCleanup at a function that releases them before exiting.
inline void (*gCheckCleanup)() = nullptr;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            if (gCheckCleanup != nullptr) {                                                    \
                gCheckCleanup();                                                               \
            }                                                                                  \
            std::fflush(stderr);                                                               \
            std::_Exit(1);                                                                     \
        }                                                                                      \
    } while (false)

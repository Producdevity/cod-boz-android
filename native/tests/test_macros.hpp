#pragma once

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_THROWS(expr)                                                                         \
    do {                                                                                           \
        bool thrown = false;                                                                       \
        try {                                                                                      \
            expr;                                                                                  \
        } catch (const std::exception &) {                                                         \
            thrown = true;                                                                         \
        }                                                                                          \
        if (!thrown) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: expected exception: %s\n", __FILE__, __LINE__,       \
                         #expr);                                                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

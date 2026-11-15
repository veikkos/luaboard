/* A minimal assert helper for the C unit tests (fw-unit). */
#pragma once

#include <stdio.h>

static int check_total;
static int check_failed;

/* Count a check; on failure print where. */
#define CHECK(cond)                                                      \
    do {                                                                 \
        check_total++;                                                   \
        if (!(cond)) {                                                   \
            check_failed++;                                              \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                \
    } while (0)

/* Control-plane domains (contract 2.2, M3-05). Standalone:
 *   gcc -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -Iinclude -Isrc \
 *       tests/rts/control/test_control_domains.c src/runtime/rts/control_domains.c -o test_control_domains
 * Covers the 14 writable names, each domain's edges, refused non-writable names and resulting-row rules. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "runtime/rts/control_domains.h"
#include "sutekh/status.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (0)

static void test_writable_set(void) {
    static const char *const want[] = {"pos_x",        "pos_y",        "speed",         "range_sq",
                                       "reload",       "damage",       "flight",        "order_mode",
                                       "order_x",      "order_y",      "formation_slot", "formation_gen",
                                       "formation_ox", "formation_oy"};
    size_t i;

    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        CHECK(su_rts_ctl_writable(want[i]) == 1);
    }
    /* Not writable: immutable kind, team, hp, and system-owned fields. */
    CHECK(su_rts_ctl_writable("kind") == 0);
    CHECK(su_rts_ctl_writable("team") == 0);
    CHECK(su_rts_ctl_writable("hp") == 0);
    CHECK(su_rts_ctl_writable("cooldown") == 0);
    CHECK(su_rts_ctl_writable("proj_target") == 0);
    CHECK(su_rts_ctl_writable(NULL) == 0);
}

static void test_bounds(void) {
    /* Each entry: field, value, expected status. */
    static const struct {
        const char *name;
        uint32_t value;
        uint32_t want;
    } cases[] = {
        {"pos_x", 0u, SU_OK},          {"pos_x", 32767u, SU_OK},       {"pos_x", 32768u, SU_E_TYPE},
        {"order_y", 32767u, SU_OK},    {"order_y", 32768u, SU_E_TYPE}, {"formation_ox", 32768u, SU_E_TYPE},
        {"speed", 0u, SU_OK},          {"speed", 1024u, SU_OK},        {"speed", 1025u, SU_E_TYPE},
        {"range_sq", 0u, SU_OK},       {"range_sq", 1u, SU_OK},        {"range_sq", 262144u, SU_OK},
        {"range_sq", 262145u, SU_E_TYPE},
        {"reload", 0u, SU_E_TYPE},     {"reload", 1u, SU_OK},          {"reload", 1024u, SU_OK},
        {"reload", 1025u, SU_E_TYPE},
        {"damage", 0u, SU_E_TYPE},     {"damage", 1u, SU_OK},          {"damage", 65535u, SU_OK},
        {"damage", 65536u, SU_E_TYPE},
        {"flight", 0u, SU_E_TYPE},     {"flight", 1u, SU_OK},          {"flight", 64u, SU_OK},
        {"flight", 65u, SU_E_TYPE},
        {"order_mode", 0u, SU_OK},     {"order_mode", 2u, SU_OK},      {"order_mode", 3u, SU_E_TYPE},
        {"formation_slot", SU_RTS_NONE, SU_OK}, {"formation_slot", 0u, SU_OK},
        {"formation_gen", 0u, SU_OK},  {"formation_gen", 0xFFFFFFFFu, SU_OK},
        {"no_such_field", 0u, SU_E_TYPE},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t st = su_rts_ctl_check_value(cases[i].name, cases[i].value);
        if (st != cases[i].want) {
            fprintf(stderr, "bound %s=%u: got %u want %u\n", cases[i].name, (unsigned)cases[i].value,
                    (unsigned)st, (unsigned)cases[i].want);
        }
        CHECK(st == cases[i].want);
    }
}

static void test_rows(void) {
    /* kind 2 requires range_sq 0. */
    CHECK(su_rts_ctl_check_row(2u, 0u, SU_RTS_NONE, 0u) == SU_OK);
    CHECK(su_rts_ctl_check_row(2u, 5u, SU_RTS_NONE, 0u) == SU_E_TYPE);
    /* formation_gen is 0 with NONE and nonzero otherwise. */
    CHECK(su_rts_ctl_check_row(1u, 0u, SU_RTS_NONE, 0u) == SU_OK);
    CHECK(su_rts_ctl_check_row(1u, 0u, SU_RTS_NONE, 3u) == SU_E_TYPE);
    CHECK(su_rts_ctl_check_row(1u, 0u, 7u, 3u) == SU_OK);
    CHECK(su_rts_ctl_check_row(1u, 0u, 7u, 0u) == SU_E_TYPE);
}

int main(void) {
    test_writable_set();
    test_bounds();
    test_rows();
    printf("test_control_domains: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

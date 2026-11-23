/* Host unit tests for src/health_tests.c. Build and run: make -C tests */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "health_tests.h"
#include "health_cutoffs.h"
#include "sim_source.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

/* Values from tools/ht_cutoffs.py, cross-checked with scipy.stats.binom.ppf. */
static void test_cutoff_table(void) {
    const unsigned rct[9] = {0, 41, 21, 15, 11, 9, 8, 7, 6};
    const unsigned apt[9] = {0, 336, 201, 123, 78, 51, 35, 26, 19};
    for (int h = 1; h <= 8; h++) {
        CHECK(HT_RCT_CUTOFF[h] == rct[h], "RCT cutoff H=%d", h);
        CHECK(HT_APT_CUTOFF[h] == apt[h], "APT cutoff H=%d", h);
    }
    ht_state_t s;
    CHECK(!ht_init_for_h(&s, 0), "H=0 rejected");
    CHECK(!ht_init_for_h(&s, 9), "H=9 rejected");
    CHECK(ht_init_for_h(&s, 4), "H=4 accepted");
    CHECK(s.rct_cutoff == 11 && s.apt_cutoff == 78 && s.apt_window == 512, "H=4 config");
}

/* RCT boundary: C-1 identical samples pass, C identical samples fail. */
static void test_rct_boundary(void) {
    ht_state_t s;
    ht_init(&s, 5, 1000, 512);
    unsigned r = 0;
    for (int i = 0; i < 4; i++) r |= ht_feed(&s, 7);
    CHECK(r == HT_OK, "4 repeats with C=5 must pass");
    r = ht_feed(&s, 7);
    CHECK(r & HT_RCT_FAIL, "5th repeat with C=5 must fail");
    CHECK(s.rct_failures == 1, "one RCT failure counted");

    /* A different value resets the run. */
    ht_init(&s, 5, 1000, 512);
    r = 0;
    for (int i = 0; i < 100; i++) {
        r |= ht_feed(&s, 7);
        r |= ht_feed(&s, 7);
        r |= ht_feed(&s, 7);
        r |= ht_feed(&s, 7);
        r |= ht_feed(&s, 8);
    }
    CHECK(r == HT_OK, "runs of 4 broken by another value must pass");
    CHECK(s.rct_longest_run == 4, "longest run tracked (%u)", s.rct_longest_run);
}

/* APT boundary: the window's first value seen C-1 times passes, C times fails. */
static void test_apt_boundary(void) {
    ht_state_t s;
    const uint32_t W = 512, C = 35;
    ht_init(&s, 1000, C, W);
    unsigned r = 0;
    /* Window 1: reference 100 appears C-1 times, spread out, other values unique. */
    uint16_t other = 1000;
    for (uint32_t i = 0; i < W; i++) {
        uint16_t x = (i % 10 == 0 && i / 10 < C - 1) ? 100 : other++;
        r |= ht_feed(&s, x);
    }
    CHECK(r == HT_OK, "C-1 occurrences in a window must pass");
    CHECK(s.apt_highest_count == C - 1, "highest count %u", s.apt_highest_count);

    /* Window 2: reference appears C times. */
    r = 0;
    other = 2000;
    for (uint32_t i = 0; i < W; i++) {
        uint16_t x = (i % 10 == 0 && i / 10 < C) ? 100 : other++;
        r |= ht_feed(&s, x);
    }
    CHECK(r & HT_APT_FAIL, "C occurrences in a window must fail");
    CHECK(s.apt_failures == 1, "one APT failure counted (%u)", s.apt_failures);

    /* The reference is the first sample of each window, not a fixed value. */
    ht_init(&s, 1000, C, W);
    r = 0;
    other = 0;
    for (uint32_t i = 0; i < W; i++) {
        uint16_t x = (i > 0 && i % 10 == 0) ? 100 : other++; /* first sample is 0 */
        r |= ht_feed(&s, x);
    }
    CHECK(r == HT_OK, "a frequent value that is not the reference is not counted");
}

static void test_startup(void) {
    static uint16_t buf[4096];
    ht_state_t s;
    ht_init_for_h(&s, 4);
    CHECK(!ht_startup(&s, buf, 1000, 0xFFF), "fewer than 1024 samples must not pass");

    sim_rng_t rng = {0x9E3779B97F4A7C15ULL, 0, 0};
    for (int i = 0; i < 4096; i++) buf[i] = sim_adc(&rng, 2061.0, 78.0);
    CHECK(ht_startup(&s, buf, 4096, 0xFFF), "healthy source passes start-up");
    CHECK(s.samples == 0, "stats reset after start-up");

    for (int i = 0; i < 4096; i++) buf[i] = 2048;
    CHECK(!ht_startup(&s, buf, 4096, 0xFFF), "stuck source fails start-up");
}

/* Mask: the firmware masks samples to 12 bits before testing. */
static void test_mask(void) {
    ht_state_t s;
    ht_init(&s, 3, 1000, 512);
    uint16_t v[3] = {0x1123, 0x2123, 0x3123}; /* differ only above bit 11 */
    CHECK(ht_feed_block(&s, v, 3, 0xFFF) & HT_RCT_FAIL, "masked values compare equal");
}

/* Healthy simulated sources must not raise false alarms. */
static void test_no_false_alarms(void) {
    const double sds[] = {78.0, 30.0};
    for (size_t k = 0; k < sizeof sds / sizeof sds[0]; k++) {
        ht_state_t s;
        ht_init_for_h(&s, 4);
        sim_rng_t rng = {0xD1B54A32D192ED03ULL + k, 0, 0};
        unsigned r = 0;
        const long n = 20000000L;
        for (long i = 0; i < n; i++) r |= ht_feed(&s, sim_adc(&rng, 2061.0, sds[k]));
        CHECK(r == HT_OK, "sd=%.0f: %u RCT, %u APT false alarms in %ld samples",
              sds[k], s.rct_failures, s.apt_failures, n);
    }
}

int main(void) {
    test_cutoff_table();
    test_rct_boundary();
    test_apt_boundary();
    test_startup();
    test_mask();
    test_no_false_alarms();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

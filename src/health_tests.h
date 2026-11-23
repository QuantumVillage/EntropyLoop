/*
 * Continuous health tests for a noise source, per NIST SP 800-90B section 4.4:
 *   - Repetition Count Test (RCT, 4.4.1): flags a run of identical samples.
 *   - Adaptive Proportion Test (APT, 4.4.2): flags one value that appears
 *     too often within a window.
 *
 * Both tests run on the raw (unhashed) samples. They catch a broken or
 * degraded source (stuck ADC, floating input, dead laser, saturation) quickly,
 * which output tests such as ent or NIST STS cannot do after hashing.
 *
 * Portable C99, no dynamic memory, no platform dependencies, so the same code
 * runs on the Pico and in host unit tests.
 */
#ifndef HEALTH_TESTS_H
#define HEALTH_TESTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Result bits returned by ht_feed() / ht_feed_block(). */
#define HT_OK       0u
#define HT_RCT_FAIL 1u
#define HT_APT_FAIL 2u

typedef struct {
    /* configuration */
    uint32_t rct_cutoff;   /* fail when a value repeats this many times in a row */
    uint32_t apt_cutoff;   /* fail when the window's first value occurs this many times */
    uint32_t apt_window;   /* APT window length in samples */

    /* RCT state */
    uint16_t rct_value;
    uint32_t rct_run;

    /* APT state */
    uint16_t apt_value;
    uint32_t apt_count;
    uint32_t apt_pos;      /* samples seen in the current window */

    /* statistics since ht_init() */
    uint64_t samples;
    uint32_t rct_failures;
    uint32_t apt_failures;
    uint32_t rct_longest_run;
    uint32_t apt_highest_count;
} ht_state_t;

/*
 * Initialise with explicit cutoffs (see tools/ht_cutoffs.py).
 */
void ht_init(ht_state_t *s, uint32_t rct_cutoff, uint32_t apt_cutoff, uint32_t apt_window);

/*
 * Initialise from a claimed min-entropy per sample, H, in whole bits (1..8),
 * using the precomputed table in health_cutoffs.h (alpha = 2^-40, W = 512).
 * Returns false if H is out of range.
 */
bool ht_init_for_h(ht_state_t *s, unsigned claimed_h_bits);

/* Feed one sample. Returns HT_OK or a mask of HT_RCT_FAIL / HT_APT_FAIL. */
unsigned ht_feed(ht_state_t *s, uint16_t sample);

/* Feed n samples. Returns the OR of all results in the block. */
unsigned ht_feed_block(ht_state_t *s, const uint16_t *samples, size_t n, uint16_t mask);

/*
 * Start-up test (SP 800-90B section 4.3, requirement 12): run both tests over
 * n >= 1024 consecutive samples. Returns true if no test failed. The caller
 * must discard these samples. Statistics are reset afterwards.
 */
bool ht_startup(ht_state_t *s, const uint16_t *samples, size_t n, uint16_t mask);

#endif /* HEALTH_TESTS_H */

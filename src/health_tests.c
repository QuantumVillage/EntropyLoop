#include "health_tests.h"
#include "health_cutoffs.h"

static void ht_reset_state(ht_state_t *s) {
    s->rct_value = 0;
    s->rct_run = 0;
    s->apt_value = 0;
    s->apt_count = 0;
    s->apt_pos = 0;
}

static void ht_reset_stats(ht_state_t *s) {
    s->samples = 0;
    s->rct_failures = 0;
    s->apt_failures = 0;
    s->rct_longest_run = 0;
    s->apt_highest_count = 0;
}

void ht_init(ht_state_t *s, uint32_t rct_cutoff, uint32_t apt_cutoff, uint32_t apt_window) {
    s->rct_cutoff = rct_cutoff;
    s->apt_cutoff = apt_cutoff;
    s->apt_window = apt_window;
    ht_reset_state(s);
    ht_reset_stats(s);
}

bool ht_init_for_h(ht_state_t *s, unsigned claimed_h_bits) {
    if (claimed_h_bits < 1 || claimed_h_bits > 8) {
        return false;
    }
    ht_init(s, HT_RCT_CUTOFF[claimed_h_bits], HT_APT_CUTOFF[claimed_h_bits], HT_APT_WINDOW);
    return true;
}

unsigned ht_feed(ht_state_t *s, uint16_t x) {
    unsigned result = HT_OK;
    s->samples++;

    /* Repetition Count Test (SP 800-90B 4.4.1) */
    if (s->rct_run > 0 && x == s->rct_value) {
        s->rct_run++;
        if (s->rct_run > s->rct_longest_run) {
            s->rct_longest_run = s->rct_run;
        }
        if (s->rct_run >= s->rct_cutoff) {
            result |= HT_RCT_FAIL;
            s->rct_failures++;
            s->rct_run = 1; /* restart the count so a stuck source keeps failing */
        }
    } else {
        s->rct_value = x;
        s->rct_run = 1;
        if (s->rct_longest_run == 0) {
            s->rct_longest_run = 1;
        }
    }

    /* Adaptive Proportion Test (SP 800-90B 4.4.2) */
    if (s->apt_pos == 0) {
        s->apt_value = x;  /* first sample of the window is the reference */
        s->apt_count = 1;
    } else if (x == s->apt_value) {
        s->apt_count++;
        if (s->apt_count >= s->apt_cutoff) {
            result |= HT_APT_FAIL;
            s->apt_failures++;
            s->apt_count = 0; /* restart the count for the rest of this window */
        }
    }
    if (s->apt_count > s->apt_highest_count) {
        s->apt_highest_count = s->apt_count;
    }
    s->apt_pos++;
    if (s->apt_pos >= s->apt_window) {
        s->apt_pos = 0;
    }

    return result;
}

unsigned ht_feed_block(ht_state_t *s, const uint16_t *samples, size_t n, uint16_t mask) {
    unsigned result = HT_OK;
    for (size_t i = 0; i < n; i++) {
        result |= ht_feed(s, (uint16_t)(samples[i] & mask));
    }
    return result;
}

bool ht_startup(ht_state_t *s, const uint16_t *samples, size_t n, uint16_t mask) {
    if (n < 1024) {
        return false;
    }
    ht_reset_state(s);
    ht_reset_stats(s);
    unsigned r = ht_feed_block(s, samples, n, mask);
    ht_reset_state(s);
    ht_reset_stats(s);
    return r == HT_OK;
}

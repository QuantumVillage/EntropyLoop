/*
 * Replay a raw capture through the health tests.
 * Input: little-endian uint16 samples (12-bit values), e.g. a treeQuantumSampler11 dump.
 *
 *   ./replay capture.bin [claimed_H=4] [max_samples=0 (all)]
 */
#include <stdio.h>
#include <stdlib.h>

#include "health_tests.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s capture.bin [claimed_H] [max_samples]\n", argv[0]);
        return 2;
    }
    unsigned h = argc > 2 ? (unsigned)atoi(argv[2]) : 4;
    unsigned long long max = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
    ht_state_t s;
    if (!ht_init_for_h(&s, h)) {
        fprintf(stderr, "claimed H must be 1..8\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    static unsigned char buf[1 << 16];
    unsigned long long first_rct = 0, first_apt = 0;
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) >= 2) {
        for (size_t i = 0; i + 1 < got; i += 2) {
            uint16_t x = (uint16_t)((buf[i] | (buf[i + 1] << 8)) & 0xFFF);
            unsigned r = ht_feed(&s, x);
            if ((r & HT_RCT_FAIL) && !first_rct) first_rct = s.samples;
            if ((r & HT_APT_FAIL) && !first_apt) first_apt = s.samples;
            if (max && s.samples >= max) goto done;
        }
    }
done:
    fclose(f);
    printf("file=%s H=%u rct_cutoff=%lu apt_cutoff=%lu/%lu samples=%llu "
           "rct_failures=%lu apt_failures=%lu longest_run=%lu highest_apt_count=%lu "
           "first_rct_fail=%llu first_apt_fail=%llu\n",
           argv[1], h, (unsigned long)s.rct_cutoff, (unsigned long)s.apt_cutoff,
           (unsigned long)s.apt_window, (unsigned long long)s.samples,
           (unsigned long)s.rct_failures, (unsigned long)s.apt_failures,
           (unsigned long)s.rct_longest_run, (unsigned long)s.apt_highest_count,
           first_rct, first_apt);
    return 0;
}

/*
 * Demo: how the health tests react to simulated healthy and faulty sources,
 * next to the firmware's existing range squelch (range < 200 per 1024 samples).
 *
 * The sources are synthetic models, not measurements of an EntropyLoop.
 * Build and run: make -C tests demo
 */
#include <stdio.h>
#include <stdlib.h>

#include "health_tests.h"
#include "sim_source.h"

#define BATCH 1024
#define N_BATCHES 10000 /* 10,240,000 samples per scenario */

typedef enum { GAUSS, STUCK, TWO_LEVEL, RAIL_CLIP } kind_t;

typedef struct {
    const char *name;
    kind_t kind;
    double mean, sd, p; /* p: fraction of samples pinned to the rail for RAIL_CLIP */
} scenario_t;

static uint16_t draw(const scenario_t *sc, sim_rng_t *r, long i) {
    switch (sc->kind) {
    case STUCK:     return (uint16_t)sc->mean;
    case TWO_LEVEL: return (i & 1) ? 4095 : 0;
    case RAIL_CLIP: return sim_uniform(r) < sc->p ? 4095 : sim_adc(r, sc->mean, sc->sd);
    default:        return sim_adc(r, sc->mean, sc->sd);
    }
}

static void run(const scenario_t *sc, unsigned h) {
    ht_state_t s;
    ht_init_for_h(&s, h);
    sim_rng_t rng = {0x2545F4914F6CDD1DULL, 0, 0};
    long first_fail = -1;
    unsigned first_kind = 0;
    long squelched = 0, ht_blocked = 0;

    for (long b = 0; b < N_BATCHES; b++) {
        uint16_t lo = 4095, hi = 0;
        unsigned r = 0;
        for (int i = 0; i < BATCH; i++) {
            uint16_t x = draw(sc, &rng, b * BATCH + i);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
            unsigned ri = ht_feed(&s, x);
            if (ri && first_fail < 0) {
                first_fail = b * BATCH + i + 1;
                first_kind = ri;
            }
            r |= ri;
        }
        if (hi - lo < 200) squelched++;
        if (r) ht_blocked++;
    }

    char when[48];
    if (first_fail < 0) {
        snprintf(when, sizeof when, "none");
    } else {
        snprintf(when, sizeof when, "%ld (%s)", first_fail,
                 (first_kind & HT_RCT_FAIL) ? "RCT" : "APT");
    }
    printf("| %-34s | %u | %-15s | %6.2f%% | %6.2f%% |\n", sc->name, h, when,
           100.0 * ht_blocked / N_BATCHES, 100.0 * squelched / N_BATCHES);
}

int main(void) {
    const scenario_t sc[] = {
        {"healthy, sd 78 (like issue #3 data)", GAUSS, 2061, 78, 0},
        {"healthy, sd 300 (wider signal)", GAUSS, 2048, 300, 0},
        {"stuck ADC (constant 2048)", STUCK, 2048, 0, 0},
        {"low noise, sd 2", GAUSS, 2048, 2, 0},
        {"low noise, sd 10", GAUSS, 2048, 10, 0},
        {"alternating 0 and 4095", TWO_LEVEL, 0, 0, 0},
        {"30% clipped at rail, sd 78", RAIL_CLIP, 2061, 78, 0.30},
        {"5% clipped at rail, sd 78", RAIL_CLIP, 2061, 78, 0.05},
    };
    printf("Samples per scenario: %d. alpha = 2^-40, APT window 512.\n\n", BATCH * N_BATCHES);
    printf("| Source (synthetic) | H | First failure at sample | Batches blocked by health tests | Batches squelched (range < 200) |\n");
    printf("|---|---|---|---|---|\n");
    for (size_t i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        run(&sc[i], 4);
        run(&sc[i], 6);
    }
    return 0;
}

/* Deterministic simulated 12-bit ADC sources for host tests. */
#ifndef SIM_SOURCE_H
#define SIM_SOURCE_H

#include <math.h>
#include <stdint.h>

typedef struct {
    uint64_t s;
    int have_spare;
    double spare;
} sim_rng_t;

static inline uint64_t sim_next(sim_rng_t *r) { /* xorshift64* */
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 2685821657736338717ULL;
}

static inline double sim_uniform(sim_rng_t *r) { /* (0,1) */
    return ((double)(sim_next(r) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

static inline double sim_gauss(sim_rng_t *r) {
    if (r->have_spare) {
        r->have_spare = 0;
        return r->spare;
    }
    double u = sim_uniform(r), v = sim_uniform(r);
    double m = sqrt(-2.0 * log(u));
    r->spare = m * sin(2.0 * M_PI * v);
    r->have_spare = 1;
    return m * cos(2.0 * M_PI * v);
}

/* Gaussian around `mean` with standard deviation `sd`, rounded and clamped to 12 bits. */
static inline uint16_t sim_adc(sim_rng_t *r, double mean, double sd) {
    long v = lround(mean + sd * sim_gauss(r));
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    return (uint16_t)v;
}

#endif

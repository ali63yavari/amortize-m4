/* selftest.c - host-side functional verification of the C gadgets.
 * Builds with: make selftest   Runs orders d = 1..8, random trials.
 * Any mismatch exits nonzero. Uses a deterministic xorshift PRNG as
 * the randomness source ON HOST ONLY; the target must use the TRNG. */
#include <stdio.h>
#include <stdlib.h>
#include "gadgets.h"

static uint32_t prng_state = 0xA2B5EEDu;
uint32_t rand32(void) {                 /* xorshift32, host only */
    uint32_t x = prng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return prng_state = x;
}

static uint32_t unshare(const uint32_t *sh, int n) {
    uint32_t a = 0;
    for (int i = 0; i < n; i++) a ^= sh[i];
    return a;
}

int main(void) {
    const uint32_t mod_mask = (1u << KS_WORDS) - 1;
    for (int d = 1; d <= 8; d++) {
        int n = d + 1;
        for (int trial = 0; trial < 5; trial++) {
            /* gadget-level checks */
            uint32_t x = rand32(), y = rand32();
            uint32_t xs[NSHARES_MAX], ys[NSHARES_MAX], zs[NSHARES_MAX];
            inject_uniform(xs, x, n);
            inject_uniform(ys, y, n);
            isw_and(zs, xs, ys, n);
            if (unshare(zs, n) != (x & y)) { puts("FAIL isw_and"); return 1; }
            refresh_sni(xs, n);
            if (unshare(xs, n) != x) { puts("FAIL refresh"); return 1; }

            /* A2B on a batch of 32 coefficients */
            uint32_t coeff[SLICE];
            uint32_t arith[NSHARES_MAX][SLICE];
            for (int c = 0; c < SLICE; c++) {
                coeff[c] = rand32() % 3329u;
                uint32_t acc = 0;
                for (int s = 0; s < n - 1; s++) {
                    arith[s][c] = rand32() & mod_mask;
                    acc = (acc + arith[s][c]) & mod_mask;
                }
                arith[n - 1][c] = (coeff[c] - acc) & mod_mask;
            }
            uint32_t out[KS_WORDS][NSHARES_MAX];
            a2b_batch32(out, (const uint32_t (*)[SLICE])arith, n);
            uint32_t outT[KS_WORDS][NSHARES_MAX];
            a2b_tree_batch32(outT, (const uint32_t (*)[SLICE])arith, n);
            for (int bit = 0; bit < K_BITS; bit++) {
                uint32_t w  = unshare(out[bit],  n);
                uint32_t wt = unshare(outT[bit], n);
                for (int c = 0; c < SLICE; c++) {
                    if (((w >> c) & 1u) != ((coeff[c] >> bit) & 1u)) {
                        printf("FAIL a2b d=%d bit=%d coeff=%d\n", d, bit, c);
                        return 1;
                    }
                    if (((wt >> c) & 1u) != ((coeff[c] >> bit) & 1u)) {
                        printf("FAIL a2b_tree d=%d bit=%d coeff=%d\n", d, bit, c);
                        return 1;
                    }
                }
            }
        }
        printf("order d=%d: all checks PASS\n", d);
    }
    puts("SELFTEST PASS");
    return 0;
}

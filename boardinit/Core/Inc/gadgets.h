/* gadgets.h - AMORTIZE masked gadget library (Cortex-M4 port skeleton)
 *
 * All gadgets operate on Boolean sharings of 32-bit words:
 *   x = sh[0] ^ sh[1] ^ ... ^ sh[d],  nshares = d + 1.
 * One word = 32 bitslices = 32 coefficients at one bit position.
 *
 * SECURITY NOTES (must hold on target):
 *  - rand32() MUST return fresh TRNG/PRG output; on STM32F407 wire it
 *    to the RNG peripheral (RNG->DR after RNG_SR_DRDY).
 *  - Compile with -O2 but AUDIT the assembly: the compiler must not
 *    recombine shares. Use the volatile barriers provided.
 *  - This skeleton targets the software probing model only; glitches
 *    and transition leakage require target-specific hardening.
 */
#ifndef AMORTIZE_GADGETS_H
#define AMORTIZE_GADGETS_H

#include <stdint.h>

#define K_BITS      12          /* coefficient width                  */
#define KS_WORDS    (K_BITS+1)  /* adder width incl. carry headroom   */
#define NSHARES_MAX 9           /* supports d <= 8                    */
#define SLICE       32          /* coefficients per word              */

extern uint32_t rand32(void);   /* fresh randomness source (TRNG)     */

/* opt barrier: prevents share-recombining optimizations */
static inline uint32_t opt_bar(uint32_t x) {
    __asm__ volatile("" : "+r"(x));
    return x;
}

void masked_xor(uint32_t *c, const uint32_t *a, const uint32_t *b, int n);
void isw_and  (uint32_t *c, const uint32_t *a, const uint32_t *b, int n);
void hpc2_and (uint32_t *c, const uint32_t *a, const uint32_t *b, int n);
void refresh_sni(uint32_t *a, int n);
void linear_refresh(uint32_t *a, int n);
void inject_uniform(uint32_t *sh, uint32_t w, int n);

/* Bitsliced masked Kogge-Stone add: A,B,S are [KS_WORDS][NSHARES_MAX] */
void ks_add(uint32_t S[KS_WORDS][NSHARES_MAX],
            uint32_t A[KS_WORDS][NSHARES_MAX],
            uint32_t B[KS_WORDS][NSHARES_MAX], int n);

/* A2B for one batch of 32 coefficients.
 * arith[s][c]: arithmetic share s of coefficient c, mod 2^KS_WORDS.
 * out[bit][share]: bitsliced Boolean sharing.                        */
void a2b_batch32(uint32_t out[KS_WORDS][NSHARES_MAX],
                 const uint32_t arith[NSHARES_MAX][SLICE], int n);

/* Tree-structured A2B (CGV14 style, Section V-D stage three).
 * Same interface as a2b_batch32. Recursive: needs about 1 KiB of
 * stack per level, depth ceil(log2(n)); reserve >= 16 KiB of stack
 * on the target. SECURITY: the reduced-share subtree argument must
 * be aligned with the CGV14 proof before production use.           */
void a2b_tree_batch32(uint32_t out[KS_WORDS][NSHARES_MAX],
                      const uint32_t arith[NSHARES_MAX][SLICE], int n);

#endif

/* gadgets.c - masked gadget implementations (see gadgets.h) */
#include <string.h>
#include "gadgets.h"

void masked_xor(uint32_t *c, const uint32_t *a, const uint32_t *b, int n) {
    for (int i = 0; i < n; i++) c[i] = opt_bar(a[i] ^ b[i]);
}

/* ISW multiplication (Ishai-Sahai-Wagner 2003), d-SNI instantiation.
 * O(n^2) ops, n(n-1)/2 fresh random words. */
void isw_and(uint32_t *c, const uint32_t *a, const uint32_t *b, int n) {
    uint32_t t[NSHARES_MAX];
    for (int i = 0; i < n; i++) t[i] = opt_bar(a[i] & b[i]);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            uint32_t r  = rand32();
            uint32_t rp = opt_bar(opt_bar(r ^ (a[i] & b[j])) ^ (a[j] & b[i]));
            t[i] = opt_bar(t[i] ^ r);
            t[j] = opt_bar(t[j] ^ rp);
        }
    }
    memcpy(c, t, (size_t)n * sizeof(uint32_t));
}

/* ISW-style SNI refresh: n(n-1)/2 randoms. */
void refresh_sni(uint32_t *a, int n) {
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            uint32_t r = rand32();
            a[i] = opt_bar(a[i] ^ r);
            a[j] = opt_bar(a[j] ^ r);
        }
}

/* Direct fresh uniform sharing of public-position word w (Section V-D).
 * n-1 randoms, n-1 XORs: linear in the order. */
void inject_uniform(uint32_t *sh, uint32_t w, int n) {
    uint32_t acc = w;
    for (int i = 0; i < n - 1; i++) {
        sh[i] = rand32();
        acc = opt_bar(acc ^ sh[i]);
    }
    sh[n - 1] = acc;
}

void ks_add(uint32_t S[KS_WORDS][NSHARES_MAX],
            uint32_t A[KS_WORDS][NSHARES_MAX],
            uint32_t B[KS_WORDS][NSHARES_MAX], int n) {
    uint32_t G[KS_WORDS][NSHARES_MAX], P[KS_WORDS][NSHARES_MAX],
             Pp[KS_WORDS][NSHARES_MAX], t[NSHARES_MAX];
    for (int i = 0; i < KS_WORDS; i++) {
        isw_and(G[i], A[i], B[i], n);
        masked_xor(P[i], A[i], B[i], n);
        memcpy(Pp[i], P[i], sizeof(P[i]));
    }
    for (int dist = 1; dist < KS_WORDS; dist <<= 1) {
        for (int i = KS_WORDS - 1; i >= dist; i--) {
            isw_and(t, Pp[i], G[i - dist], n);
            masked_xor(G[i], G[i], t, n);
        }
        for (int i = KS_WORDS - 1; i >= 2 * dist; i--)
            isw_and(Pp[i], Pp[i], Pp[i - dist], n);
    }
    memcpy(S[0], P[0], sizeof(S[0]));
    for (int i = 1; i < KS_WORDS; i++)
        masked_xor(S[i], P[i], G[i - 1], n);
}

/* Linear refresh: n-1 randoms. Used at tree merges (CGV14 style). */
void linear_refresh(uint32_t *a, int n) {
    for (int i = 0; i < n - 1; i++) {
        uint32_t r = rand32();
        a[i]     = opt_bar(a[i] ^ r);
        a[n - 1] = opt_bar(a[n - 1] ^ r);
    }
}

static void transpose32(uint32_t *words, const uint32_t *coeffs) {
    for (int bit = 0; bit < KS_WORDS; bit++) {
        uint32_t w = 0;
        for (int c = 0; c < SLICE; c++)
            w |= ((coeffs[c] >> bit) & 1u) << c;
        words[bit] = w;
    }
}

static void tree_rec(uint32_t out[KS_WORDS][NSHARES_MAX],
                     const uint32_t arith[NSHARES_MAX][SLICE],
                     int lo, int hi) {
    int cnt = hi - lo;
    if (cnt == 1) {
        uint32_t w[KS_WORDS];
        transpose32(w, arith[lo]);
        for (int bit = 0; bit < KS_WORDS; bit++) {
            memset(out[bit], 0, sizeof(out[bit]));
            out[bit][0] = w[bit];
        }
        return;
    }
    int mid = lo + cnt / 2;
    int szL = mid - lo, szR = hi - mid;
    uint32_t L[KS_WORDS][NSHARES_MAX], R[KS_WORDS][NSHARES_MAX];
    tree_rec(L, arith, lo, mid);
    tree_rec(R, arith, mid, hi);
    for (int bit = 0; bit < KS_WORDS; bit++) {
        for (int s = szL; s < cnt; s++) L[bit][s] = 0;   /* expand */
        for (int s = szR; s < cnt; s++) R[bit][s] = 0;
        linear_refresh(L[bit], cnt);                      /* re-randomize */
        linear_refresh(R[bit], cnt);
    }
    ks_add(out, L, R, cnt);                               /* single merge */
}

void a2b_tree_batch32(uint32_t out[KS_WORDS][NSHARES_MAX],
                      const uint32_t arith[NSHARES_MAX][SLICE], int n) {
    tree_rec(out, arith, 0, n);
}

void a2b_batch32(uint32_t out[KS_WORDS][NSHARES_MAX],
                 const uint32_t arith[NSHARES_MAX][SLICE], int n) {
    uint32_t cur[KS_WORDS][NSHARES_MAX], nxt[KS_WORDS][NSHARES_MAX];
    for (int s = 0; s < n; s++) {
        uint32_t (*dst)[NSHARES_MAX] = (s == 0) ? cur : nxt;
        for (int bit = 0; bit < KS_WORDS; bit++) {
            uint32_t w = 0;
            for (int c = 0; c < SLICE; c++)
                w |= ((arith[s][c] >> bit) & 1u) << c;   /* transpose */
            inject_uniform(dst[bit], w, n);
        }
        if (s > 0) {
            ks_add(out, cur, nxt, n);
            memcpy(cur, out, sizeof(cur));
        }
    }
    memcpy(out, cur, sizeof(cur));
}

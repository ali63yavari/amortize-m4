/*
 * bench_step2_audit.c
 *
 * Audit benchmark for AMORTIZE Step 2 on STM32F407 / Cortex-M4.
 *
 * Purpose:
 *   - Prove that the benchmark is actually executing on the board.
 *   - Record raw per-run DWT cycle counts.
 *   - Record TRNG call counts.
 *   - Record estimated RNG wait cycles.
 *   - Record output checksum so calls cannot be optimized away silently.
 *   - Optionally record stack high-water usage.
 *
 * Serial output contains two CSV blocks:
 *
 *   BEGIN_M4_CYCLES_RAW
 *   variant,order,run,cycles_per_poly,rng_calls,rng_wait_cycles,checksum,stack_bytes
 *   ...
 *   END_M4_CYCLES_RAW
 *
 *   BEGIN_M4_CYCLES_SUMMARY
 *   variant,order,runs,median_cycles_per_poly,cycles_per_coeff,stack_bytes
 *   ...
 *   END_M4_CYCLES_SUMMARY
 *
 * Notes:
 *   - This audit version adds overhead because rand32() is instrumented.
 *   - Use this to defend measurement authenticity.
 *   - Use the clean non-audit benchmark for final manuscript cycle numbers.
 */

#include "gadgets.h"
#include "main.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern UART_HandleTypeDef huart2;

/* ============================================================
 * User configuration
 * ============================================================
 */

#ifndef BENCH_ENABLE_STACK_WATERMARK
#define BENCH_ENABLE_STACK_WATERMARK 1
#endif

#define BENCH_ORDER_MIN 1
#define BENCH_ORDER_MAX 8

#define BENCH_WARMUP_RUNS 1
#define BENCH_MEASURED_RUNS 11

#define BENCH_BATCHES_PER_RUN 8
#define BENCH_COEFFS_PER_RUN (BENCH_BATCHES_PER_RUN * SLICE)

#define STACK_WATERMARK_PATTERN 0xA5A5A5A5u
#define STACK_GUARD_BYTES 256u

/* ============================================================
 * DWT cycle counter
 * ============================================================
 */

#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)

static void cyccnt_init(void) {
  DEMCR |= (1u << 24);
  DWT_CYCCNT = 0;
  DWT_CTRL |= 1u;
}

static inline uint32_t cyccnt_read(void) { return DWT_CYCCNT; }

/* ============================================================
 * RNG instrumentation
 * ============================================================
 */

#define RNG_BASE_ADDR 0x50060800u
#define RNG_CR_REG (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x00u))
#define RNG_SR_REG (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x04u))
#define RNG_DR_REG (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x08u))

#define RNG_CR_RNGEN (1u << 2)
#define RNG_SR_DRDY (1u << 0)
#define RNG_SR_CECS (1u << 1)
#define RNG_SR_SECS (1u << 2)

static volatile uint32_t g_rng_calls = 0;
static volatile uint32_t g_rng_wait_cycles = 0;
static volatile uint32_t g_rng_error_flags = 0;

static void rng_init_direct(void) {
  __HAL_RCC_RNG_CLK_ENABLE();
  RNG_CR_REG |= RNG_CR_RNGEN;
}

static void rng_audit_reset(void) {
  g_rng_calls = 0;
  g_rng_wait_cycles = 0;
  g_rng_error_flags = 0;
}

static uint32_t rng_audit_calls(void) { return g_rng_calls; }

static uint32_t rng_audit_wait_cycles(void) { return g_rng_wait_cycles; }

static uint32_t rng_audit_error_flags(void) { return g_rng_error_flags; }

/*
 * gadgets.c expects this exact symbol.
 *
 * This audit rand32() intentionally adds instrumentation overhead.
 * Therefore, the resulting cycle numbers are audit-cycle numbers,
 * not clean final manuscript cycle numbers.
 */
uint32_t rand32(void) {
  g_rng_calls++;

  uint32_t t0 = cyccnt_read();

  while ((RNG_SR_REG & RNG_SR_DRDY) == 0u) {
    uint32_t sr = RNG_SR_REG;

    if ((sr & (RNG_SR_CECS | RNG_SR_SECS)) != 0u) {
      g_rng_error_flags |= sr & (RNG_SR_CECS | RNG_SR_SECS);
    }
  }

  uint32_t t1 = cyccnt_read();

  g_rng_wait_cycles += (t1 - t0);

  return RNG_DR_REG;
}

/* ============================================================
 * USART2 output helpers
 * ============================================================
 */

static void uart_write(const char *s) {
  HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

static void uart_write_char(char c) {
  HAL_UART_Transmit(&huart2, (uint8_t *)&c, 1, HAL_MAX_DELAY);
}

static void uart_write_u32(uint32_t v) {
  char buf[11];
  int i = 0;

  if (v == 0u) {
    uart_write_char('0');
    return;
  }

  while (v > 0u && i < (int)sizeof(buf)) {
    buf[i++] = (char)('0' + (v % 10u));
    v /= 10u;
  }

  while (i > 0) {
    uart_write_char(buf[--i]);
  }
}

static void uart_write_hex32(uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";

  uart_write("0x");

  for (int i = 7; i >= 0; i--) {
    uint32_t nibble = (v >> (4u * (uint32_t)i)) & 0xFu;
    uart_write_char(hex[nibble]);
  }
}

static void uart_write_cycles_per_coeff(uint32_t median_cycles) {
  uint32_t whole = median_cycles / BENCH_COEFFS_PER_RUN;
  uint32_t rem = median_cycles % BENCH_COEFFS_PER_RUN;
  uint32_t frac = (rem * 1000u) / BENCH_COEFFS_PER_RUN;

  uart_write_u32(whole);
  uart_write_char('.');

  if (frac < 100u)
    uart_write_char('0');
  if (frac < 10u)
    uart_write_char('0');

  uart_write_u32(frac);
}

/* ============================================================
 * Sorting and median
 * ============================================================
 */

static void sort_u32(uint32_t *a, uint32_t n) {
  for (uint32_t i = 1; i < n; i++) {
    uint32_t key = a[i];
    int32_t j = (int32_t)i - 1;

    while (j >= 0 && a[j] > key) {
      a[j + 1] = a[j];
      j--;
    }

    a[j + 1] = key;
  }
}

static uint32_t median_u32(uint32_t *a, uint32_t n) {
  sort_u32(a, n);
  return a[n / 2u];
}

/* ============================================================
 * Optional stack watermarking
 * ============================================================
 */

#if BENCH_ENABLE_STACK_WATERMARK

extern uint32_t __StackTop;
extern uint32_t __StackLimit;

static uint32_t *g_stack_fill_start = 0;
static uint32_t *g_stack_fill_end = 0;

static inline uint32_t get_msp(void) {
  uint32_t sp;
  __asm volatile("mrs %0, msp" : "=r"(sp));
  return sp;
}

static void stack_watermark_begin(void) {
  uintptr_t stack_limit = (uintptr_t)&__StackLimit;
  uintptr_t stack_top = (uintptr_t)&__StackTop;
  uintptr_t current_sp = (uintptr_t)get_msp();

  if (stack_top <= stack_limit) {
    g_stack_fill_start = 0;
    g_stack_fill_end = 0;
    return;
  }

  if (current_sp <= stack_limit + STACK_GUARD_BYTES) {
    g_stack_fill_start = 0;
    g_stack_fill_end = 0;
    return;
  }

  if (current_sp > stack_top) {
    g_stack_fill_start = 0;
    g_stack_fill_end = 0;
    return;
  }

  uintptr_t fill_start = stack_limit;
  uintptr_t fill_end = current_sp - STACK_GUARD_BYTES;

  fill_start = (fill_start + 3u) & ~(uintptr_t)3u;
  fill_end = fill_end & ~(uintptr_t)3u;

  if (fill_end <= fill_start) {
    g_stack_fill_start = 0;
    g_stack_fill_end = 0;
    return;
  }

  g_stack_fill_start = (uint32_t *)fill_start;
  g_stack_fill_end = (uint32_t *)fill_end;

  for (uint32_t *p = g_stack_fill_start; p < g_stack_fill_end; p++) {
    *p = STACK_WATERMARK_PATTERN;
  }

  __asm volatile("" ::: "memory");
}

static uint32_t stack_watermark_end(void) {
  if (g_stack_fill_start == 0 || g_stack_fill_end == 0) {
    return 0u;
  }

  __asm volatile("" ::: "memory");

  uint32_t *lowest_changed = g_stack_fill_end;

  for (uint32_t *p = g_stack_fill_start; p < g_stack_fill_end; p++) {
    if (*p != STACK_WATERMARK_PATTERN) {
      lowest_changed = p;
      break;
    }
  }

  if (lowest_changed == g_stack_fill_end) {
    return 0u;
  }

  return (uint32_t)((uintptr_t)g_stack_fill_end - (uintptr_t)lowest_changed);
}

#else

static void stack_watermark_begin(void) {}

static uint32_t stack_watermark_end(void) { return 0u; }

#endif

/* ============================================================
 * Input generation and checksum
 * ============================================================
 */

static void fill_arith_random(uint32_t arith[NSHARES_MAX][SLICE], int n) {
  const uint32_t mask = (1u << KS_WORDS) - 1u;

  for (int s = 0; s < n; s++) {
    for (int c = 0; c < SLICE; c++) {
      arith[s][c] = rand32() & mask;
    }
  }
}

static uint32_t checksum_out(uint32_t out[KS_WORDS][NSHARES_MAX]) {
  uint32_t acc = 0x12345678u;

  for (int bit = 0; bit < KS_WORDS; bit++) {
    for (int s = 0; s < NSHARES_MAX; s++) {
      uint32_t x = out[bit][s];

      acc ^= x + 0x9E3779B9u + (acc << 6) + (acc >> 2);
    }
  }

  return acc;
}

/*
 * Prevent compiler from proving that the result is unused.
 */
static volatile uint32_t g_checksum_sink = 0;

/* ============================================================
 * Benchmark body
 * ============================================================
 */

typedef enum { VARIANT_SEQ = 0, VARIANT_TREE = 1 } bench_variant_t;

typedef struct {
  uint32_t cycles;
  uint32_t rng_calls;
  uint32_t rng_wait_cycles;
  uint32_t rng_error_flags;
  uint32_t checksum;
  uint32_t stack_bytes;
} bench_sample_t;

typedef struct {
  uint32_t median_cycles;
  uint32_t max_stack_bytes;
} bench_summary_t;

static bench_sample_t run_one_measurement(bench_variant_t variant, int n) {
  static uint32_t arith[NSHARES_MAX][SLICE];
  static uint32_t out[KS_WORDS][NSHARES_MAX];

  bench_sample_t result;
  result.cycles = 0u;
  result.rng_calls = 0u;
  result.rng_wait_cycles = 0u;
  result.rng_error_flags = 0u;
  result.checksum = 0u;
  result.stack_bytes = 0u;

  /*
   * Generate input outside the timed region.
   * We reset RNG audit AFTER this, so rng_calls in the raw CSV count
   * only the randomness consumed by the conversion itself.
   */
  fill_arith_random(arith, n);

  rng_audit_reset();

  stack_watermark_begin();

  DWT_CYCCNT = 0;

  __disable_irq();

  uint32_t t0 = cyccnt_read();

  for (int rep = 0; rep < BENCH_BATCHES_PER_RUN; rep++) {
    if (variant == VARIANT_SEQ) {
      a2b_batch32(out, (const uint32_t (*)[SLICE])arith, n);
    } else {
      a2b_tree_batch32(out, (const uint32_t (*)[SLICE])arith, n);
    }
  }

  uint32_t t1 = cyccnt_read();

  __enable_irq();

  /*
   * Checksum is intentionally outside the timed region.
   * It proves output changed and prevents dead-code elimination,
   * but it does not pollute the cycle count.
   */
  result.checksum = checksum_out(out);
  g_checksum_sink ^= result.checksum;

  result.cycles = t1 - t0;
  result.rng_calls = rng_audit_calls();
  result.rng_wait_cycles = rng_audit_wait_cycles();
  result.rng_error_flags = rng_audit_error_flags();
  result.stack_bytes = stack_watermark_end();

  return result;
}

/* ============================================================
 * CSV output
 * ============================================================
 */

static const char *variant_name(bench_variant_t variant) {
  return (variant == VARIANT_SEQ) ? "seq" : "tree";
}

static void print_raw_header(void) {
  uart_write("BEGIN_M4_CYCLES_RAW\r\n");
  uart_write("variant,order,run,cycles_per_poly,rng_calls,rng_wait_cycles,rng_"
             "error_flags,checksum,stack_bytes\r\n");
}

static void print_raw_footer(void) { uart_write("END_M4_CYCLES_RAW\r\n"); }

static void print_summary_header(void) {
  uart_write("BEGIN_M4_CYCLES_SUMMARY\r\n");
  uart_write("variant,order,runs,median_cycles_per_poly,cycles_per_coeff,stack_"
             "bytes\r\n");
}

static void print_summary_footer(void) {
  uart_write("END_M4_CYCLES_SUMMARY\r\n");
}

static void print_raw_row(bench_variant_t variant, int order, int run_index,
                          const bench_sample_t *sample) {
  uart_write(variant_name(variant));
  uart_write_char(',');

  uart_write_u32((uint32_t)order);
  uart_write_char(',');

  uart_write_u32((uint32_t)run_index);
  uart_write_char(',');

  uart_write_u32(sample->cycles);
  uart_write_char(',');

  uart_write_u32(sample->rng_calls);
  uart_write_char(',');

  uart_write_u32(sample->rng_wait_cycles);
  uart_write_char(',');

  uart_write_hex32(sample->rng_error_flags);
  uart_write_char(',');

  uart_write_hex32(sample->checksum);
  uart_write_char(',');

#if BENCH_ENABLE_STACK_WATERMARK
  uart_write_u32(sample->stack_bytes);
#else
  uart_write("NA");
#endif

  uart_write("\r\n");
}

static void print_summary_row(bench_variant_t variant, int order, uint32_t runs,
                              uint32_t median_cycles, uint32_t stack_bytes) {
  uart_write(variant_name(variant));
  uart_write_char(',');

  uart_write_u32((uint32_t)order);
  uart_write_char(',');

  uart_write_u32(runs);
  uart_write_char(',');

  uart_write_u32(median_cycles);
  uart_write_char(',');

  uart_write_cycles_per_coeff(median_cycles);
  uart_write_char(',');

#if BENCH_ENABLE_STACK_WATERMARK
  uart_write_u32(stack_bytes);
#else
  uart_write("NA");
#endif

  uart_write("\r\n");
}

/* ============================================================
 * Per-variant/order benchmark
 * ============================================================
 */

static bench_summary_t benchmark_variant_order(bench_variant_t variant,
                                               int order) {
  const int n = order + 1;

  uint32_t cycle_samples[BENCH_MEASURED_RUNS];
  uint32_t max_stack = 0u;

  /*
   * Warm-up, discarded and not printed as measured data.
   */
  for (int i = 0; i < BENCH_WARMUP_RUNS; i++) {
    (void)run_one_measurement(variant, n);
  }

  for (int i = 0; i < BENCH_MEASURED_RUNS; i++) {
    bench_sample_t s = run_one_measurement(variant, n);

    cycle_samples[i] = s.cycles;

    if (s.stack_bytes > max_stack) {
      max_stack = s.stack_bytes;
    }

    print_raw_row(variant, order, i + 1, &s);
  }

  bench_summary_t summary;
  summary.median_cycles = median_u32(cycle_samples, BENCH_MEASURED_RUNS);
  summary.max_stack_bytes = max_stack;

  return summary;
}

/* ============================================================
 * Public entry point
 * ============================================================
 */

void bench_step2_audit_run(void) {
  bench_summary_t summaries[2][BENCH_ORDER_MAX + 1];

  cyccnt_init();
  rng_init_direct();

  HAL_Delay(500);

  /*
   * Optional debug sanity lines.
   * These are comments, not CSV rows. The Python splitter should ignore them.
   */
  uart_write("# bench_step2_audit.c\r\n");
  uart_write("# HCLK=");
  uart_write_u32(HAL_RCC_GetHCLKFreq());
  uart_write("\r\n");
  uart_write("# coeffs_per_run=");
  uart_write_u32(BENCH_COEFFS_PER_RUN);
  uart_write("\r\n");

#if BENCH_ENABLE_STACK_WATERMARK
  uart_write("# stack_watermark=enabled\r\n");
#else
  uart_write("# stack_watermark=disabled\r\n");
#endif

  print_raw_header();

  for (int order = BENCH_ORDER_MIN; order <= BENCH_ORDER_MAX; order++) {
    HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12);

    summaries[VARIANT_SEQ][order] = benchmark_variant_order(VARIANT_SEQ, order);

    HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_13);

    summaries[VARIANT_TREE][order] =
        benchmark_variant_order(VARIANT_TREE, order);
  }

  print_raw_footer();

  print_summary_header();

  for (int order = BENCH_ORDER_MIN; order <= BENCH_ORDER_MAX; order++) {
    print_summary_row(VARIANT_SEQ, order, BENCH_MEASURED_RUNS,
                      summaries[VARIANT_SEQ][order].median_cycles,
                      summaries[VARIANT_SEQ][order].max_stack_bytes);

    print_summary_row(VARIANT_TREE, order, BENCH_MEASURED_RUNS,
                      summaries[VARIANT_TREE][order].median_cycles,
                      summaries[VARIANT_TREE][order].max_stack_bytes);
  }

  print_summary_footer();

  uart_write("DONE\r\n");

  while (1) {
    HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12 | GPIO_PIN_13);
    HAL_Delay(500);
  }
}

void print_clock(void) {
  uart_write("# HCLK=");
  uart_write_u32(HAL_RCC_GetHCLKFreq());
  uart_write("\r\n");

  uart_write("# RCC_CR=");
  uart_write_hex32(RCC->CR);
  uart_write("\r\n");

  uart_write("# RCC_PLLCFGR=");
  uart_write_hex32(RCC->PLLCFGR);
  uart_write("\r\n");

  uart_write("# RCC_CFGR=");
  uart_write_hex32(RCC->CFGR);
  uart_write("\r\n");
}

static volatile uint32_t g_rng_sink = 0;

void rng_direct_stress_test(void) {
  cyccnt_init();
  rng_init_direct();

  uart_write((char *)"BEGIN_RNG_STRESS\r\n");
  uart_write("idx,value,spins,read_path_cycles,error_flags\r\n");

  for (uint32_t i = 0; i < 64; i++) {
    uint32_t spins = 0;

    uint32_t t0 = cyccnt_read();

    while ((RNG_SR_REG & RNG_SR_DRDY) == 0u) {
      spins++;
    }

    uint32_t value = RNG_DR_REG;

    uint32_t t1 = cyccnt_read();

    uint32_t error_flags = RNG_SR_REG & (RNG_SR_CECS | RNG_SR_SECS);

    g_rng_sink ^= value;

    uart_write_u32(i);
    uart_write_char(',');

    uart_write_hex32(value);
    uart_write_char(',');

    uart_write_u32(spins);
    uart_write_char(',');

    uart_write_u32(t1 - t0);
    uart_write_char(',');

    uart_write_hex32(error_flags);
    uart_write("\r\n");
  }

  uart_write("END_RNG_STRESS\r\n");
}

void rng_direct_stress_test_v2(void) {
  cyccnt_init();
  rng_init_direct();

  uart_write("BEGIN_RNG_STRESS_V2\r\n");
  uart_write("idx,sr_before,value,sr_after,spins_until_next_ready,cycles_until_"
             "next_ready,error_flags_after\r\n");

  for (uint32_t i = 0; i < 64; i++) {
    /*
     * Wait for the current word to be ready.
     */
    while ((RNG->SR & RNG_SR_DRDY) == 0u) {
    }

    uint32_t sr_before = RNG->SR;
    uint32_t value = RNG->DR;
    uint32_t sr_after = RNG->SR;

    /*
     * Now test whether DRDY clears and how long it takes to become ready again.
     */
    uint32_t spins = 0u;

    uint32_t t0 = cyccnt_read();

    while ((RNG->SR & RNG_SR_DRDY) == 0u) {
      spins++;
    }

    uint32_t t1 = cyccnt_read();

    uint32_t error_flags_after = RNG->SR & (RNG_SR_CECS | RNG_SR_SECS);

    g_rng_sink ^= value;

    uart_write_u32(i);
    uart_write_char(',');

    uart_write_hex32(sr_before);
    uart_write_char(',');

    uart_write_hex32(value);
    uart_write_char(',');

    uart_write_hex32(sr_after);
    uart_write_char(',');

    uart_write_u32(spins);
    uart_write_char(',');

    uart_write_u32(t1 - t0);
    uart_write_char(',');

    uart_write_hex32(error_flags_after);
    uart_write("\r\n");
  }

  uart_write("END_RNG_STRESS_V2\r\n");
}
/*
 * bench_step2_stack.c
 *
 * Step 2 Cortex-M4 benchmark for AMORTIZE conversion-layer cycle counts.
 *
 * Target:
 *   STM32F407 / Cortex-M4
 *
 * Output over USART2:
 *
 *   variant,order,runs,median_cycles_per_poly,cycles_per_coeff,stack_bytes
 *   seq,1,11,...
 *   tree,1,11,...
 *
 * Measurement protocol:
 *   - order d = 1..8
 *   - nshares = d + 1
 *   - 1 warm-up run, discarded
 *   - 11 measured runs
 *   - median cycles reported
 *   - each measured run performs 8 batches
 *   - each batch processes 32 coefficients
 *   - therefore cycles_per_coeff = median_cycles / 256
 *
 * Stack measurement:
 *   - controlled by BENCH_ENABLE_STACK_WATERMARK
 *   - if enabled, stack_bytes is measured by stack watermarking
 *   - if disabled, stack_bytes is printed as NA
 *
 * IMPORTANT:
 *   For final manuscript numbers, run the MCU at HCLK = 24 MHz,
 *   compile this file and gadgets.c with -O2, and reserve at least
 *   16 KiB stack in the linker script.
 */

#include "main.h"
#include "gadgets.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * CubeMX-generated USART2 handle.
 * Your project must already initialize USART2 before calling bench_step2_stack_run().
 */
extern UART_HandleTypeDef huart2;

/* ============================================================
 * User configuration
 * ============================================================
 */

/*
 * Set to 1 to print real stack_bytes using stack watermarking.
 * Set to 0 to print NA for stack_bytes.
 *
 * If this is 1, your linker script must provide:
 *
 *   __StackTop
 *   __StackLimit
 *
 * See the linker-script note below.
 */
#ifndef BENCH_ENABLE_STACK_WATERMARK
#define BENCH_ENABLE_STACK_WATERMARK 1
#endif

#define BENCH_ORDER_MIN        1
#define BENCH_ORDER_MAX        8

/*
 * One warm-up run is executed and discarded.
 * Then 11 measured runs are collected and the median is reported.
 */
#define BENCH_WARMUP_RUNS      1
#define BENCH_MEASURED_RUNS    11

/*
 * a2b_batch32() and a2b_tree_batch32() process 32 coefficients per call.
 * 8 calls = 256 coefficients.
 */
#define BENCH_BATCHES_PER_RUN  8
#define BENCH_COEFFS_PER_RUN   (BENCH_BATCHES_PER_RUN * SLICE)

/*
 * Used only when BENCH_ENABLE_STACK_WATERMARK = 1.
 */
#define STACK_WATERMARK_PATTERN  0xA5A5A5A5u
#define STACK_GUARD_BYTES        256u

/* ============================================================
 * DWT cycle counter
 * ============================================================
 */

#define DWT_CTRL       (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT     (*(volatile uint32_t *)0xE0001004u)
#define DEMCR          (*(volatile uint32_t *)0xE000EDFCu)

static void cyccnt_init(void)
{
    /*
     * Enable trace/debug block.
     */
    DEMCR |= (1u << 24);

    /*
     * Reset and enable DWT cycle counter.
     */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;
}

static inline uint32_t cyccnt_read(void)
{
    return DWT_CYCCNT;
}

/* ============================================================
 * RNG direct-register implementation for STM32F407
 *
 * gadgets.c expects:
 *
 *   uint32_t rand32(void);
 *
 * Final measurement should use real RNG, not deterministic rand().
 *
 * Clock requirement:
 *   RNG needs a valid 48 MHz clock.
 *   If you use the 24 MHz HCLK config we discussed:
 *
 *     HSI = 16 MHz
 *     PLLM = 16
 *     PLLN = 192
 *     PLLP = 8   -> SYSCLK/HCLK = 24 MHz
 *     PLLQ = 4   -> PLL48CLK = 48 MHz
 *
 * then RNG is OK.
 * ============================================================
 */

#define RNG_BASE_ADDR  0x50060800u
#define RNG_CR_REG     (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x00u))
#define RNG_SR_REG     (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x04u))
#define RNG_DR_REG     (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x08u))

// is defined in stm32f407xx.h
// #define RNG_CR_RNGEN   (1u << 2)
// #define RNG_SR_DRDY    (1u << 0)
// #define RNG_SR_CECS    (1u << 1)
// #define RNG_SR_SECS    (1u << 2)

static void rng_init_direct(void)
{
    __HAL_RCC_RNG_CLK_ENABLE();

    /*
     * Clear and enable RNG.
     */
    RNG_CR_REG |= RNG_CR_RNGEN;
}

uint32_t rand32(void)
{
    /*
     * If this loop hangs, RNG clocking is wrong.
     * Check PLL48CLK / PLLQ and __HAL_RCC_RNG_CLK_ENABLE().
     */
    while ((RNG_SR_REG & RNG_SR_DRDY) == 0u)
    {
        /*
         * Optional error handling could check CECS/SECS here.
         */
    }

    return RNG_DR_REG;
}

/* ============================================================
 * USART2 output helpers
 * ============================================================
 */

static void uart_write(const char *s)
{
    HAL_UART_Transmit(&huart2,
                      (uint8_t *)s,
                      (uint16_t)strlen(s),
                      HAL_MAX_DELAY);
}

static void uart_write_char(char c)
{
    HAL_UART_Transmit(&huart2,
                      (uint8_t *)&c,
                      1,
                      HAL_MAX_DELAY);
}

static void uart_write_u32(uint32_t v)
{
    char buf[11];
    int i = 0;

    if (v == 0u)
    {
        uart_write_char('0');
        return;
    }

    while (v > 0u && i < (int)sizeof(buf))
    {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (i > 0)
    {
        uart_write_char(buf[--i]);
    }
}

static void uart_write_cycles_per_coeff(uint32_t median_cycles)
{
    /*
     * Print median_cycles / 256 with 3 decimal digits.
     * Avoids float printf.
     *
     * Example:
     *   123456 / 256 = 482.250
     */
    uint32_t whole = median_cycles / BENCH_COEFFS_PER_RUN;
    uint32_t rem   = median_cycles % BENCH_COEFFS_PER_RUN;
    uint32_t frac  = (rem * 1000u) / BENCH_COEFFS_PER_RUN;

    uart_write_u32(whole);
    uart_write_char('.');

    if (frac < 100u) uart_write_char('0');
    if (frac < 10u)  uart_write_char('0');

    uart_write_u32(frac);
}

/* ============================================================
 * Sorting and median
 * ============================================================
 */

static void sort_u32(uint32_t *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
    {
        uint32_t key = a[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0 && a[j] > key)
        {
            a[j + 1] = a[j];
            j--;
        }

        a[j + 1] = key;
    }
}

static uint32_t median_u32(uint32_t *a, uint32_t n)
{
    sort_u32(a, n);
    return a[n / 2u];
}

/* ============================================================
 * Optional stack watermarking
 * ============================================================
 */

#if BENCH_ENABLE_STACK_WATERMARK

/*
 * Linker script must define these symbols.
 *
 * Add this to your linker script after MEMORY and after _Min_Stack_Size:
 *
 *   _Min_Stack_Size = 0x4000;
 *
 *   __StackTop = ORIGIN(RAM) + LENGTH(RAM);
 *   __StackLimit = __StackTop - _Min_Stack_Size;
 *   PROVIDE(__StackTop = __StackTop);
 *   PROVIDE(__StackLimit = __StackLimit);
 *
 * On STM32F407VG, RAM is usually:
 *
 *   RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
 */
extern uint32_t __StackTop;
extern uint32_t __StackLimit;

static uint32_t *g_stack_fill_start = 0;
static uint32_t *g_stack_fill_end   = 0;

static inline uint32_t get_msp(void)
{
    uint32_t sp;
    __asm volatile ("mrs %0, msp" : "=r" (sp));
    return sp;
}

static void stack_watermark_begin(void)
{
    uintptr_t stack_limit = (uintptr_t)&__StackLimit;
    uintptr_t stack_top   = (uintptr_t)&__StackTop;
    uintptr_t current_sp  = (uintptr_t)get_msp();

    /*
     * Basic sanity checks.
     */
    if (stack_top <= stack_limit)
    {
        g_stack_fill_start = 0;
        g_stack_fill_end = 0;
        return;
    }

    if (current_sp <= stack_limit + STACK_GUARD_BYTES)
    {
        g_stack_fill_start = 0;
        g_stack_fill_end = 0;
        return;
    }

    if (current_sp > stack_top)
    {
        g_stack_fill_start = 0;
        g_stack_fill_end = 0;
        return;
    }

    /*
     * Stack grows downward.
     *
     * We fill only the currently-unused lower stack region:
     *
     *   [__StackLimit, current_sp - STACK_GUARD_BYTES)
     *
     * We must not overwrite active stack frames, so keep a guard below
     * current_sp.
     */
    uintptr_t fill_start = stack_limit;
    uintptr_t fill_end   = current_sp - STACK_GUARD_BYTES;

    /*
     * Word align.
     */
    fill_start = (fill_start + 3u) & ~(uintptr_t)3u;
    fill_end   = fill_end & ~(uintptr_t)3u;

    if (fill_end <= fill_start)
    {
        g_stack_fill_start = 0;
        g_stack_fill_end = 0;
        return;
    }

    g_stack_fill_start = (uint32_t *)fill_start;
    g_stack_fill_end   = (uint32_t *)fill_end;

    for (uint32_t *p = g_stack_fill_start; p < g_stack_fill_end; p++)
    {
        *p = STACK_WATERMARK_PATTERN;
    }

    /*
     * Compiler barrier.
     */
    __asm volatile ("" ::: "memory");
}

static uint32_t stack_watermark_end(void)
{
    if (g_stack_fill_start == 0 || g_stack_fill_end == 0)
    {
        return 0u;
    }

    __asm volatile ("" ::: "memory");

    /*
     * Stack grows downward.
     *
     * During the benchmark, deeper stack usage overwrites the upper suffix
     * of the filled region:
     *
     *   [deepest_sp, g_stack_fill_end)
     *
     * Find the first changed word from the low end.
     */
    uint32_t *lowest_changed = g_stack_fill_end;

    for (uint32_t *p = g_stack_fill_start; p < g_stack_fill_end; p++)
    {
        if (*p != STACK_WATERMARK_PATTERN)
        {
            lowest_changed = p;
            break;
        }
    }

    if (lowest_changed == g_stack_fill_end)
    {
        return 0u;
    }

    return (uint32_t)((uintptr_t)g_stack_fill_end -
                      (uintptr_t)lowest_changed);
}

#else

static void stack_watermark_begin(void)
{
}

static uint32_t stack_watermark_end(void)
{
    return 0u;
}

#endif

/* ============================================================
 * Benchmark input generation
 * ============================================================
 */

static void fill_arith_random(uint32_t arith[NSHARES_MAX][SLICE], int n)
{
    /*
     * Arithmetic shares are represented as KS_WORDS-bit values.
     * KS_WORDS = 13, so mask = 0x1FFF.
     */
    const uint32_t mask = (1u << KS_WORDS) - 1u;

    for (int s = 0; s < n; s++)
    {
        for (int c = 0; c < SLICE; c++)
        {
            arith[s][c] = rand32() & mask;
        }
    }
}

/* ============================================================
 * Timed benchmark body
 * ============================================================
 */

typedef enum
{
    VARIANT_SEQ = 0,
    VARIANT_TREE = 1
} bench_variant_t;

typedef struct
{
    uint32_t cycles;
    uint32_t stack_bytes;
} bench_sample_t;

typedef struct
{
    uint32_t median_cycles;
    uint32_t max_stack_bytes;
} bench_result_t;

static bench_sample_t run_one_measurement(bench_variant_t variant, int n)
{
    /*
     * Static buffers reduce stack pressure outside the gadget implementation.
     * The tree conversion still uses recursive local arrays inside gadgets.c,
     * so stack usage is still meaningful and must be measured/reserved.
     */
    static uint32_t arith[NSHARES_MAX][SLICE];
    static uint32_t out[KS_WORDS][NSHARES_MAX];

    bench_sample_t result;
    result.cycles = 0u;
    result.stack_bytes = 0u;

    /*
     * Generate fresh random arithmetic shares outside timed region.
     * This keeps input setup and RNG generation out of the cycle count.
     */
    fill_arith_random(arith, n);

    /*
     * Watermark unused stack immediately before timed execution.
     */
    stack_watermark_begin();

    /*
     * Reset cycle counter immediately before timing.
     */
    //DWT_CYCCNT = 0;

    /*
     * Disable interrupts so SysTick does not pollute the timed region.
     */
    __disable_irq();

    uint32_t t0 = cyccnt_read();

    for (int rep = 0; rep < BENCH_BATCHES_PER_RUN; rep++)
    {
        if (variant == VARIANT_SEQ)
        {
            a2b_batch32(out,
                        (const uint32_t (*)[SLICE])arith,
                        n);
        }
        else
        {
            a2b_tree_batch32(out,
                             (const uint32_t (*)[SLICE])arith,
                             n);
        }
    }

    uint32_t t1 = cyccnt_read();

    __enable_irq();

    result.cycles = t1 - t0;
    result.stack_bytes = stack_watermark_end();

    return result;
}

static bench_result_t benchmark_variant_order(bench_variant_t variant, int order)
{
    const int n = order + 1;

    uint32_t cycle_samples[BENCH_MEASURED_RUNS];
    uint32_t max_stack = 0u;

    /*
     * Warm-up run, not counted.
     */
    for (int i = 0; i < BENCH_WARMUP_RUNS; i++)
    {
        (void)run_one_measurement(variant, n);
    }

    /*
     * Measured runs.
     */
    for (int i = 0; i < BENCH_MEASURED_RUNS; i++)
    {
        bench_sample_t s = run_one_measurement(variant, n);

        cycle_samples[i] = s.cycles;

        /*
         * For stack usage, worst-case across measured runs is more useful
         * than median.
         */
        if (s.stack_bytes > max_stack)
        {
            max_stack = s.stack_bytes;
        }
    }

    bench_result_t r;
    r.median_cycles = median_u32(cycle_samples, BENCH_MEASURED_RUNS);
    r.max_stack_bytes = max_stack;

    return r;
}

/* ============================================================
 * CSV output
 * ============================================================
 */

static void print_csv_row(const char *variant,
                          int order,
                          uint32_t runs,
                          uint32_t median_cycles,
                          uint32_t stack_bytes)
{
    uart_write(variant);
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
    (void)stack_bytes;
    uart_write("NA");
#endif

    uart_write("\r\n");
}

/* ============================================================
 * Public entry point
 * ============================================================
 */

void bench_step2_stack_run(void)
{
    cyccnt_init();
    rng_init_direct();

    /*
     * Give serial terminal a short time after reset.
     */
    HAL_Delay(500);

    /*
     * Optional sanity line.
     *
     * Keep this commented for final CSV capture.
     * Uncomment only when debugging clock setup.
     */
    /*
    uart_write("HCLK=");
    uart_write_u32(HAL_RCC_GetHCLKFreq());
    uart_write("\r\n");
    */

    uart_write("variant,order,runs,median_cycles_per_poly,cycles_per_coeff,stack_bytes\r\n");

    for (int order = BENCH_ORDER_MIN; order <= BENCH_ORDER_MAX; order++)
    {
        /*
         * Optional progress indicator on STM32F407 Discovery LEDs.
         * If PD12/PD13 are not configured, remove these two calls.
         */
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12);

        bench_result_t seq_result =
            benchmark_variant_order(VARIANT_SEQ, order);

        print_csv_row("seq",
                      order,
                      BENCH_MEASURED_RUNS,
                      seq_result.median_cycles,
                      seq_result.max_stack_bytes);

        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_13);

        bench_result_t tree_result =
            benchmark_variant_order(VARIANT_TREE, order);

        print_csv_row("tree",
                      order,
                      BENCH_MEASURED_RUNS,
                      tree_result.median_cycles,
                      tree_result.max_stack_bytes);
    }

    uart_write("DONE\r\n");

    while (1)
    {
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12 | GPIO_PIN_13);
        HAL_Delay(500);
    }
}


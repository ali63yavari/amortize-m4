#include "main.h"
#include "gadgets.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern UART_HandleTypeDef huart2;


/* ============================================================
 * Benchmark configuration
 * ============================================================
 */

#define BENCH_ORDER_MIN        1
#define BENCH_ORDER_MAX        8

/*
 * One physical warm-up run is executed and discarded.
 * Then 11 measured runs are collected.
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
 * Set this to 0 for the first clean run.
 * Later we can implement real stack watermarking and put real values here.
 */
#define BENCH_STACK_BYTES_UNKNOWN  0u

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
     * Reset and enable cycle counter.
     */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;
}

static inline uint32_t cyccnt_read(void)
{
    return DWT_CYCCNT;
}

/* ============================================================
 * STM32F407 RNG direct-register implementation
 *
 * This requires PLL48CLK = 48 MHz.
 * The 24 MHz SystemClock_Config above gives PLL48CLK through PLLQ = 4.
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
    /*
     * Enable RNG peripheral clock on AHB2.
     */
    __HAL_RCC_RNG_CLK_ENABLE();

    /*
     * Enable RNG.
     */
    RNG_CR_REG |= RNG_CR_RNGEN;
}

/*
 * gadgets.c expects this exact symbol.
 * It is declared in gadgets.h as:
 *
 *     extern uint32_t rand32(void);
 */
uint32_t rand32(void)
{
    /*
     * For final measurements, do not replace this with rand(), LCG, or fixed values.
     * The masking gadgets use randomness internally.
     */
    while ((RNG_SR_REG & RNG_SR_DRDY) == 0u)
    {
        /*
         * If this hangs, PLL48CLK/RNG clock is not configured correctly.
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

static void uart_write_u32(uint32_t v)
{
    char buf[11];
    int i = 0;

    if (v == 0u)
    {
        uart_write("0");
        return;
    }

    while (v > 0u && i < (int)sizeof(buf))
    {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (i > 0)
    {
        char c = buf[--i];
        HAL_UART_Transmit(&huart2, (uint8_t *)&c, 1, HAL_MAX_DELAY);
    }
}

static void uart_write_cycles_per_coeff(uint32_t median_cycles)
{
    /*
     * Print median_cycles / 256 as decimal with 3 digits.
     * Avoids float printf.
     *
     * Example:
     *   123456 / 256 = 482.250
     */
    uint32_t whole = median_cycles / BENCH_COEFFS_PER_RUN;
    uint32_t rem   = median_cycles % BENCH_COEFFS_PER_RUN;
    uint32_t frac  = (rem * 1000u) / BENCH_COEFFS_PER_RUN;

    uart_write_u32(whole);
    uart_write(".");

    if (frac < 100u) uart_write("0");
    if (frac < 10u)  uart_write("0");

    uart_write_u32(frac);
}

/* ============================================================
 * Small sort + median for 11 values
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
 * Input generation
 * ============================================================
 */

static void fill_arith_random(uint32_t arith[NSHARES_MAX][SLICE], int n)
{
    /*
     * Arithmetic shares are represented as KS_WORDS-bit values.
     * KS_WORDS = 13 in gadgets.h, so mask = 0x1FFF.
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

static uint32_t run_one_measurement(bench_variant_t variant, int n)
{
    /*
     * Keep these static to reduce benchmark stack pressure.
     * The tree conversion itself still uses recursion and local arrays
     * inside gadgets.c, so linker stack must still be increased.
     */
    static uint32_t arith[NSHARES_MAX][SLICE];
    static uint32_t out[KS_WORDS][NSHARES_MAX];

    fill_arith_random(arith, n);

    /*
     * Reset cycle counter immediately before the timed region.
     */
    DWT_CYCCNT = 0;

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

    return t1 - t0;
}

static uint32_t benchmark_median_cycles(bench_variant_t variant, int order)
{
    const int n = order + 1;

    uint32_t samples[BENCH_MEASURED_RUNS];

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
        samples[i] = run_one_measurement(variant, n);
    }

    return median_u32(samples, BENCH_MEASURED_RUNS);
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
    uart_write(",");

    uart_write_u32((uint32_t)order);
    uart_write(",");

    uart_write_u32(runs);
    uart_write(",");

    uart_write_u32(median_cycles);
    uart_write(",");

    uart_write_cycles_per_coeff(median_cycles);
    uart_write(",");

    if (stack_bytes == 0u)
    {
        uart_write("NA");
    }
    else
    {
        uart_write_u32(stack_bytes);
    }

    uart_write("\r\n");
}

/* ============================================================
 * Public entry point called from main.c
 * ============================================================
 */

void bench_step2_run(void)
{
    cyccnt_init();
    rng_init_direct();

    /*
     * Give serial terminal a moment after reset.
     */
    HAL_Delay(500);

    /*
     * Final CSV header requested by the manuscript task.
     */
    uart_write("variant,order,runs,median_cycles_per_poly,cycles_per_coeff,stack_bytes\r\n");

    for (int order = BENCH_ORDER_MIN; order <= BENCH_ORDER_MAX; order++)
    {
        /*
         * Optional visual progress on Discovery LEDs.
         */
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12);

        uint32_t seq_median = benchmark_median_cycles(VARIANT_SEQ, order);

        print_csv_row("seq",
                      order,
                      BENCH_MEASURED_RUNS,
                      seq_median,
                      BENCH_STACK_BYTES_UNKNOWN);

        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_13);

        uint32_t tree_median = benchmark_median_cycles(VARIANT_TREE, order);

        print_csv_row("tree",
                      order,
                      BENCH_MEASURED_RUNS,
                      tree_median,
                      BENCH_STACK_BYTES_UNKNOWN);
    }

    uart_write("DONE\r\n");

    while (1)
    {
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12 | GPIO_PIN_13);
        HAL_Delay(500);
    }
}
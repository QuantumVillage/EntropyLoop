// ============================================================================
// FiberRanger Build 3.1a — AD7606 Drift-Resistant QRNG Firmware
// ============================================================================
//
// Fixes over 3.1:
//   1. AD7606 BUSY wait: now waits for HIGH then LOW (not just LOW)
//   2. 2us guard delay after BUSY drops before SPI read
//   3. Paced sampling loop at configurable target SPS (default 10 kSPS)
//   4. Raw stats per batch: CH1/CH2/DIFF min/max/spread
//   5. Diff computed as int32_t (int16 subtraction can overflow)
//
// Preserved from 3.1:
//   - AD7606 hardware oversampling (OS0-OS2 on GP10-12), runtime adjustable
//   - RANGE pin under firmware control (GP13)
//   - Software DC blocker (rolling 256-sample mean)
//   - Von Neumann debias on DIFF
//   - Rolling health window + drift warnings
//   - Runtime serial commands
// ============================================================================

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "mbedtls/sha512.h"
#include "squarewave.pio.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define TARGET_FREQ_KHZ      250000
#define PIO_PIN              0

#define ADC_PIN              26
#define ADC_INPUT            0

#define SPI_PORT             spi0
#define SPI_MISO             16
#define SPI_SCK              18
#define SPI_CS               17

#define AD7606_CONVST        20
#define AD7606_BUSY          21
#define AD7606_RST           22

#define AD7606_OS0           10
#define AD7606_OS1           11
#define AD7606_OS2           12
#define AD7606_RANGE         13

#define OVERSAMPLE_DEFAULT   2   // 4x

// Paced sampling — NOT a hammer loop
#define AD7606_TARGET_SPS        10000
#define AD7606_SAMPLE_PERIOD_US  (1000000 / AD7606_TARGET_SPS)

// BUSY handshake timing
#define BUSY_HIGH_TIMEOUT_US 50
#define BUSY_LOW_TIMEOUT_US  5000
#define BUSY_GUARD_DELAY_US  2

#define BATCH_SIZE           1024
#define DIFF_BATCH_SIZE      1024
#define LAG_DEPTH            12
#define BATCH_WINDOW_US      200000

#define DC_WINDOW            256
#define LED_PIN              25
#define VERSION_STR          "v3.1a-busy-wait-paced"

// ============================================================================
// SHARED STATE
// ============================================================================

typedef struct {
    uint16_t int_samples[BATCH_SIZE];
    int32_t  diff_samples[DIFF_BATCH_SIZE];
    int16_t  ch1_samples[DIFF_BATCH_SIZE];
    int16_t  ch2_samples[DIFF_BATCH_SIZE];
    uint16_t int_count;
    uint16_t diff_count;
    uint32_t seq;
    uint32_t timestamp_ms;
    char     buf_id;
    uint32_t ad7606_timeouts;
} adc_batch_t;

queue_t sample_queue;

static uint16_t dma_buf_a[BATCH_SIZE];
static uint16_t dma_buf_b[BATCH_SIZE];
static int dma_chan_a, dma_chan_b;
static volatile bool buf_a_done = false;
static volatile bool buf_b_done = false;

static bool ad7606_ok = false;
static uint8_t current_oversample = OVERSAMPLE_DEFAULT;
static uint32_t ad7606_total_timeouts = 0;

static int32_t diff_dc_history[DC_WINDOW];
static int32_t int_dc_history[DC_WINDOW];
static int diff_dc_idx = 0;
static int int_dc_idx = 0;
static int64_t diff_dc_sum = 0;
static int64_t int_dc_sum = 0;
static bool diff_dc_primed = false;
static bool int_dc_primed = false;

#define HEALTH_WINDOW 10
static float diff_h_history[HEALTH_WINDOW];
static float int_h_history[HEALTH_WINDOW];
static int health_idx = 0;
static bool health_primed = false;

static volatile uint32_t global_seq = 0;

// ============================================================================
// DC BLOCKER
// ============================================================================

static int32_t dc_block_diff(int32_t sample) {
    int32_t old = diff_dc_history[diff_dc_idx];
    diff_dc_history[diff_dc_idx] = sample;
    diff_dc_sum += sample - old;
    diff_dc_idx = (diff_dc_idx + 1) % DC_WINDOW;
    if (!diff_dc_primed) {
        if (diff_dc_idx == 0) diff_dc_primed = true;
        return 0;
    }
    int32_t mean = (int32_t)(diff_dc_sum / DC_WINDOW);
    return sample - mean;
}

static int32_t dc_block_int(int32_t sample) {
    int32_t old = int_dc_history[int_dc_idx];
    int_dc_history[int_dc_idx] = sample;
    int_dc_sum += sample - old;
    int_dc_idx = (int_dc_idx + 1) % DC_WINDOW;
    if (!int_dc_primed) {
        if (int_dc_idx == 0) int_dc_primed = true;
        return 0;
    }
    int32_t mean = (int32_t)(int_dc_sum / DC_WINDOW);
    return sample - mean;
}

// ============================================================================
// AD7606 DRIVER
// ============================================================================

static void ad7606_set_oversample(uint8_t ratio) {
    if (ratio > 6) ratio = 6;
    gpio_put(AD7606_OS0, (ratio >> 0) & 1);
    gpio_put(AD7606_OS1, (ratio >> 1) & 1);
    gpio_put(AD7606_OS2, (ratio >> 2) & 1);
    current_oversample = ratio;
}

static void ad7606_reset(void) {
    gpio_put(AD7606_RST, 1);
    sleep_us(50);
    gpio_put(AD7606_RST, 0);
    sleep_us(50);
}

static void ad7606_start_conversion(void) {
    gpio_put(AD7606_CONVST, 0);
    sleep_us(1);
    gpio_put(AD7606_CONVST, 1);
    sleep_us(1);
    gpio_put(AD7606_CONVST, 0);
}

// Proper BUSY handshake:
//   1. Wait for BUSY to go HIGH (conversion started)
//   2. Wait for BUSY to go LOW (conversion complete)
//   3. Guard delay before SPI read
static bool ad7606_wait_conversion(void) {
    // Step 1: BUSY HIGH
    uint32_t i = 0;
    while (!gpio_get(AD7606_BUSY)) {
        if (++i > BUSY_HIGH_TIMEOUT_US) {
            // BUSY never asserted — maybe the pin isn't wired. Fall back to blind delay.
            sleep_us(BUSY_LOW_TIMEOUT_US);
            sleep_us(BUSY_GUARD_DELAY_US);
            return true;
        }
        sleep_us(1);
    }
    // Step 2: BUSY LOW
    i = 0;
    while (gpio_get(AD7606_BUSY)) {
        if (++i > BUSY_LOW_TIMEOUT_US) return false;
        sleep_us(1);
    }
    // Step 3: guard delay
    sleep_us(BUSY_GUARD_DELAY_US);
    return true;
}

static void ad7606_read_channels(int16_t *ch1, int16_t *ch2) {
    uint8_t buf[4];
    gpio_put(SPI_CS, 0);
    spi_read_blocking(SPI_PORT, 0x00, buf, 4);
    gpio_put(SPI_CS, 1);
    *ch1 = (int16_t)((buf[0] << 8) | buf[1]);
    *ch2 = (int16_t)((buf[2] << 8) | buf[3]);
}

static bool ad7606_init(void) {
    spi_init(SPI_PORT, 10000000);
    gpio_set_function(SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SCK,  GPIO_FUNC_SPI);

    gpio_init(SPI_CS); gpio_set_dir(SPI_CS, GPIO_OUT); gpio_put(SPI_CS, 1);
    gpio_init(AD7606_CONVST); gpio_set_dir(AD7606_CONVST, GPIO_OUT); gpio_put(AD7606_CONVST, 0);
    gpio_init(AD7606_RST);    gpio_set_dir(AD7606_RST, GPIO_OUT);    gpio_put(AD7606_RST, 0);
    gpio_init(AD7606_BUSY);   gpio_set_dir(AD7606_BUSY, GPIO_IN);

    gpio_init(AD7606_OS0); gpio_set_dir(AD7606_OS0, GPIO_OUT);
    gpio_init(AD7606_OS1); gpio_set_dir(AD7606_OS1, GPIO_OUT);
    gpio_init(AD7606_OS2); gpio_set_dir(AD7606_OS2, GPIO_OUT);
    ad7606_set_oversample(OVERSAMPLE_DEFAULT);

    gpio_init(AD7606_RANGE); gpio_set_dir(AD7606_RANGE, GPIO_OUT); gpio_put(AD7606_RANGE, 0);

    ad7606_reset();
    sleep_ms(10);

    ad7606_start_conversion();
    if (!ad7606_wait_conversion()) {
        printf("BOOT: AD7606 BUSY never dropped — check wiring\n");
        return false;
    }
    int16_t t1, t2;
    ad7606_read_channels(&t1, &t2);
    printf("BOOT: AD7606 OK (OS=%dx, RANGE=+/-5V, target=%d SPS, first=%d,%d)\n",
           1 << current_oversample, AD7606_TARGET_SPS, t1, t2);
    return true;
}

// ============================================================================
// INTERNAL ADC DMA PING-PONG
// ============================================================================

static void dma_handler_a(void) {
    dma_hw->ints0 = 1u << dma_chan_a;
    buf_a_done = true;
    dma_channel_set_write_addr(dma_chan_a, dma_buf_a, false);
    dma_channel_set_trans_count(dma_chan_a, BATCH_SIZE, true);
}

static void dma_handler_b(void) {
    dma_hw->ints1 = 1u << dma_chan_b;
    buf_b_done = true;
    dma_channel_set_write_addr(dma_chan_b, dma_buf_b, false);
    dma_channel_set_trans_count(dma_chan_b, BATCH_SIZE, true);
}

static void int_adc_init(void) {
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_INPUT);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(96.0f);

    dma_chan_a = dma_claim_unused_channel(true);
    dma_chan_b = dma_claim_unused_channel(true);

    dma_channel_config cfg_a = dma_channel_get_default_config(dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, dma_chan_b);

    dma_channel_config cfg_b = dma_channel_get_default_config(dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, dma_chan_a);

    dma_channel_configure(dma_chan_a, &cfg_a,
        dma_buf_a, &adc_hw->fifo, BATCH_SIZE, false);
    dma_channel_configure(dma_chan_b, &cfg_b,
        dma_buf_b, &adc_hw->fifo, BATCH_SIZE, false);

    dma_channel_set_irq0_enabled(dma_chan_a, true);
    dma_channel_set_irq1_enabled(dma_chan_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler_a);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_handler_b);
    irq_set_enabled(DMA_IRQ_0, true);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_start(dma_chan_a);
    adc_run(true);
}

// ============================================================================
// CORE 1 — ENTROPY PIPELINE
// ============================================================================

static float compute_hmin(const int32_t *samples, int count,
                          int lag_depth, int bin_size, int num_bins) {
    if (count <= lag_depth + 2) return 0.0f;

    int *counts = (int*)calloc(num_bins, sizeof(int));
    if (!counts) return 0.0f;

    int n_deltas = 0;
    for (int i = lag_depth; i < count; i++) {
        int32_t d = samples[i] - samples[i - lag_depth];
        int bin = (d / bin_size) + (num_bins / 2);
        if (bin >= 0 && bin < num_bins) {
            counts[bin]++;
            n_deltas++;
        }
    }

    if (n_deltas == 0) { free(counts); return 0.0f; }

    int max_count = 0;
    for (int i = 0; i < num_bins; i++) {
        if (counts[i] > max_count) max_count = counts[i];
    }
    free(counts);

    if (max_count == 0) return 0.0f;
    float p_max = (float)max_count / (float)n_deltas;
    float h_min = -log2f(p_max);

    h_min *= (8.0f / log2f((float)n_deltas));
    if (h_min > 8.0f) h_min = 8.0f;
    return h_min;
}

static int von_neumann_debias(const int32_t *samples, int count,
                              uint8_t *out_bytes, int max_bytes) {
    int bit_idx = 0;
    int byte_idx = 0;
    uint8_t cur_byte = 0;

    for (int i = 0; i + 1 < count; i += 2) {
        int b1 = (samples[i] > 0) ? 1 : 0;
        int b2 = (samples[i+1] > 0) ? 1 : 0;
        if (b1 == b2) continue;
        int vn_bit = b1;
        cur_byte = (cur_byte << 1) | vn_bit;
        bit_idx++;
        if (bit_idx == 8) {
            if (byte_idx >= max_bytes) return byte_idx;
            out_bytes[byte_idx++] = cur_byte;
            bit_idx = 0;
            cur_byte = 0;
        }
    }
    return byte_idx;
}

static void sha512_hash(const uint8_t *in, size_t len, uint8_t out[64]) {
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    mbedtls_sha512_update(&ctx, in, len);
    mbedtls_sha512_finish(&ctx, out);
    mbedtls_sha512_free(&ctx);
}

static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) printf("%02x", data[i]);
}

static void int16_min_max(const int16_t *arr, int n, int16_t *mn, int16_t *mx) {
    if (n == 0) { *mn = 0; *mx = 0; return; }
    *mn = arr[0]; *mx = arr[0];
    for (int i = 1; i < n; i++) {
        if (arr[i] < *mn) *mn = arr[i];
        if (arr[i] > *mx) *mx = arr[i];
    }
}

static void int32_min_max(const int32_t *arr, int n, int32_t *mn, int32_t *mx) {
    if (n == 0) { *mn = 0; *mx = 0; return; }
    *mn = arr[0]; *mx = arr[0];
    for (int i = 1; i < n; i++) {
        if (arr[i] < *mn) *mn = arr[i];
        if (arr[i] > *mx) *mx = arr[i];
    }
}

static void core1_entry(void) {
    static adc_batch_t batch;
    static int32_t diff_dc[DIFF_BATCH_SIZE];
    static int32_t int_dc[BATCH_SIZE];
    uint8_t vn_out[128];
    uint8_t hash_diff[64];
    uint8_t hash_int[64];

    while (true) {
        queue_remove_blocking(&sample_queue, &batch);

        int diff_n = 0;
        int int_n = 0;
        for (int i = 0; i < batch.diff_count; i++) {
            diff_dc[diff_n++] = dc_block_diff(batch.diff_samples[i]);
        }
        for (int i = 0; i < batch.int_count; i++) {
            int_dc[int_n++] = dc_block_int((int32_t)batch.int_samples[i]);
        }

        float diff_h = compute_hmin(diff_dc, diff_n, LAG_DEPTH, 1, 512);
        float int_h  = compute_hmin(int_dc,  int_n,  LAG_DEPTH, 1, 512);

        int64_t diff_r_sum = 0;
        for (int i = 0; i < diff_n; i++) diff_r_sum += abs(diff_dc[i]);
        int32_t diff_r = (diff_n > 0) ? (int32_t)(diff_r_sum / diff_n) : 0;

        int64_t int_r_sum = 0;
        for (int i = 0; i < int_n; i++) int_r_sum += abs(int_dc[i]);
        int32_t int_r = (int_n > 0) ? (int32_t)(int_r_sum / int_n) : 0;

        // RAW STATS — min/max/spread per channel
        int16_t ch1_min, ch1_max, ch2_min, ch2_max;
        int32_t diff_min, diff_max;
        int16_min_max(batch.ch1_samples, batch.diff_count, &ch1_min, &ch1_max);
        int16_min_max(batch.ch2_samples, batch.diff_count, &ch2_min, &ch2_max);
        int32_min_max(batch.diff_samples, batch.diff_count, &diff_min, &diff_max);

        int vn_bytes = von_neumann_debias(diff_dc, diff_n, vn_out, sizeof(vn_out));

        sha512_hash((uint8_t*)diff_dc, diff_n * sizeof(int32_t), hash_diff);
        sha512_hash((uint8_t*)int_dc,  int_n  * sizeof(int32_t), hash_int);

        diff_h_history[health_idx] = diff_h;
        int_h_history[health_idx]  = int_h;
        health_idx = (health_idx + 1) % HEALTH_WINDOW;
        if (health_idx == 0) health_primed = true;

        float diff_h_min_win = diff_h, diff_h_max_win = diff_h;
        float int_h_min_win  = int_h,  int_h_max_win  = int_h;
        if (health_primed) {
            diff_h_min_win = diff_h_max_win = diff_h_history[0];
            int_h_min_win  = int_h_max_win  = int_h_history[0];
            for (int i = 1; i < HEALTH_WINDOW; i++) {
                if (diff_h_history[i] < diff_h_min_win) diff_h_min_win = diff_h_history[i];
                if (diff_h_history[i] > diff_h_max_win) diff_h_max_win = diff_h_history[i];
                if (int_h_history[i]  < int_h_min_win)  int_h_min_win  = int_h_history[i];
                if (int_h_history[i]  > int_h_max_win)  int_h_max_win  = int_h_history[i];
            }
        }

        const char *drift_warn = "";
        if (health_primed && (diff_h_max_win - diff_h_min_win) > 1.5f) {
            drift_warn = " | DRIFT:HIGH";
        }

        printf("SEQ:%lu BUF:%c T:%lums | "
               "DIFF H:%.4f R:%ld N:%d HMIN10:%.2f HMAX10:%.2f | "
               "INT H:%.4f R:%ld N:%d HMIN10:%.2f HMAX10:%.2f | "
               "VN:%d OS:%dx TO:%lu%s\n",
               batch.seq, batch.buf_id, batch.timestamp_ms,
               diff_h, diff_r, diff_n, diff_h_min_win, diff_h_max_win,
               int_h, int_r, int_n, int_h_min_win, int_h_max_win,
               vn_bytes, 1 << current_oversample, batch.ad7606_timeouts, drift_warn);

        printf("RAW | CH1:%d..%d sp=%d | CH2:%d..%d sp=%d | DIFF:%ld..%ld sp=%ld | N:%d\n",
               ch1_min, ch1_max, ch1_max - ch1_min,
               ch2_min, ch2_max, ch2_max - ch2_min,
               diff_min, diff_max, diff_max - diff_min,
               batch.diff_count);

        print_hex(hash_diff, 64); printf("\n");
        print_hex(hash_int,  64); printf("\n");
        if (vn_bytes > 0) {
            printf("VN:");
            print_hex(vn_out, vn_bytes);
            printf("\n");
        }
        fflush(stdout);

        gpio_put(LED_PIN, !gpio_get(LED_PIN));
    }
}

// ============================================================================
// SERIAL COMMAND HANDLER
// ============================================================================

static void handle_serial_cmd(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) return;

    switch (c) {
        case '0': case '1': case '2': case '3': case '4': case '5': case '6':
            ad7606_set_oversample(c - '0');
            printf("CMD: oversample set to %dx\n", 1 << current_oversample);
            break;
        case 'r':
            gpio_put(AD7606_RANGE, 0);
            printf("CMD: range +/-5V (RANGE=LOW)\n");
            break;
        case 'R':
            gpio_put(AD7606_RANGE, 1);
            printf("CMD: range +/-10V (RANGE=HIGH)\n");
            break;
        case 'd':
            memset(diff_dc_history, 0, sizeof(diff_dc_history));
            memset(int_dc_history,  0, sizeof(int_dc_history));
            diff_dc_sum = int_dc_sum = 0;
            diff_dc_idx = int_dc_idx = 0;
            diff_dc_primed = int_dc_primed = false;
            printf("CMD: DC blocker reset\n");
            break;
        case 's':
            printf("CMD: target_sps=%d period=%dus OS=%dx total_timeouts=%lu\n",
                   AD7606_TARGET_SPS, AD7606_SAMPLE_PERIOD_US,
                   1 << current_oversample, ad7606_total_timeouts);
            break;
        case 'h':
            printf("CMD: [0-6]=OS 1x..64x  r/R=range  d=reset DC  s=stats  h=help  v=version\n");
            break;
        case 'v':
            printf("CMD: firmware %s\n", VERSION_STR);
            break;
        default: break;
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(TARGET_FREQ_KHZ, true);

    stdio_init_all();
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\n\nBOOT: FiberRanger %s\n", VERSION_STR);
    printf("BOOT: sysclk=%lu Hz\n", clock_get_hz(clk_sys));

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &squarewave_program);
    squarewave_program_init(pio, sm, offset, PIO_PIN);
    printf("BOOT: PIO square wave on GP%d OK\n", PIO_PIN);

    int_adc_init();
    printf("BOOT: INT ADC DMA ping-pong OK\n");

    ad7606_ok = ad7606_init();
    if (!ad7606_ok) {
        printf("BOOT: AD7606 not responding — continuing INT-only\n");
    }

    queue_init(&sample_queue, sizeof(adc_batch_t), 4);

    multicore_launch_core1(core1_entry);
    printf("BOOT: Core 1 launched\n");
    printf("BOOT: target AD7606 SPS=%d (period=%dus)\n",
           AD7606_TARGET_SPS, AD7606_SAMPLE_PERIOD_US);
    printf("BOOT: main loop entering — send 'h' for runtime commands\n");
    fflush(stdout);

    // ============================================================================
    // CORE 0 — ACQUISITION LOOP (paced)
    // ============================================================================

    static adc_batch_t current;
    memset(&current, 0, sizeof(current));
    absolute_time_t batch_deadline = make_timeout_time_us(BATCH_WINDOW_US);
    absolute_time_t next_ad7606_sample = get_absolute_time();
    uint32_t boot_time_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        handle_serial_cmd();

        // Grab INT buffer if ready
        if (current.int_count == 0) {
            if (buf_a_done) {
                memcpy(current.int_samples, dma_buf_a, BATCH_SIZE * sizeof(uint16_t));
                current.int_count = BATCH_SIZE;
                current.buf_id = 'A';
                buf_a_done = false;
            } else if (buf_b_done) {
                memcpy(current.int_samples, dma_buf_b, BATCH_SIZE * sizeof(uint16_t));
                current.int_count = BATCH_SIZE;
                current.buf_id = 'B';
                buf_b_done = false;
            }
        }

        // Paced AD7606 sampling — not a hammer loop
        if (ad7606_ok &&
            current.diff_count < DIFF_BATCH_SIZE &&
            absolute_time_diff_us(get_absolute_time(), next_ad7606_sample) <= 0) {

            ad7606_start_conversion();

            if (ad7606_wait_conversion()) {
                int16_t ch1, ch2;
                ad7606_read_channels(&ch1, &ch2);
                int32_t diff = (int32_t)ch1 - (int32_t)ch2;  // int32 — no overflow

                int idx = current.diff_count;
                current.ch1_samples[idx]  = ch1;
                current.ch2_samples[idx]  = ch2;
                current.diff_samples[idx] = diff;
                current.diff_count++;
            } else {
                current.ad7606_timeouts++;
                ad7606_total_timeouts++;
            }

            next_ad7606_sample = delayed_by_us(next_ad7606_sample,
                                               AD7606_SAMPLE_PERIOD_US);
        }

        // Ship batch on deadline OR when both channels full
        bool deadline_hit = absolute_time_diff_us(get_absolute_time(), batch_deadline) <= 0;
        bool both_full = (current.int_count == BATCH_SIZE) &&
                         (current.diff_count == DIFF_BATCH_SIZE);

        if (deadline_hit || both_full) {
            if (current.int_count > 0 || current.diff_count > 0) {
                current.seq = global_seq++;
                current.timestamp_ms = to_ms_since_boot(get_absolute_time()) - boot_time_ms;
                queue_add_blocking(&sample_queue, &current);
            }
            memset(&current, 0, sizeof(current));
            batch_deadline = make_timeout_time_us(BATCH_WINDOW_US);
        }
    }

    return 0;
}

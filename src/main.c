#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"

// Crypto Library
#include "mbedtls/sha512.h"

#include "squarewave.pio.h"

// ----------------------------------------------------------------------------
// CONFIGURATION
// ----------------------------------------------------------------------------
#define TARGET_FREQ_KHZ   250000  // 250 MHz System Clock
#define PIO_PIN           0       // Square Wave Output
#define ADC_PIN           26      // ADC Input (GPIO 26)
#define ADC_INPUT         0       // ADC Channel 0 matches GPIO 26
#define BATCH_SIZE        1024    // Samples per batch
#define LAG_DEPTH         12      // Compare current sample vs 4/8/12 samples ago

// Structure to pass data between cores
typedef struct {
    uint16_t samples[BATCH_SIZE];
} adc_batch_t;

// Thread-safe queue for inter-core communication
queue_t sample_queue;

// ----------------------------------------------------------------------------
// CORE 1: Processing & Output
// ----------------------------------------------------------------------------

// Helper to print bytes as Hex
void print_hex(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

// Wrapper to allow swapping SHA-512 out easily
void crypto_hash(const unsigned char *input, size_t ilen, unsigned char *output) {
    mbedtls_sha512(input, ilen, output, 0); 
}

void core1_entry() {
    static adc_batch_t batch;
    uint8_t hash_out_1[64];
    uint8_t hash_out_2[64];
    
    // Histogram for Min-Entropy
    // Moved to static to prevent Core 1 stack overflow (Default stack is 2KB)
    static uint16_t counts[4096]; 

    // --- RING BUFFER HISTORY ---
    // We store the last 'LAG_DEPTH' samples here.
    // Comparing T vs T-12 breaks the correlation of the slow laser pulse shape.
    static uint16_t history[LAG_DEPTH] = {0}; 
    static uint8_t hist_head = 0;
    // ---------------------------

    while (true) {
        // 1. Wait for data from Core 0
        queue_remove_blocking(&sample_queue, &batch);

        // Reset diagnostics
        memset(counts, 0, sizeof(counts));
        uint16_t max_count = 0;
        
        // --- Range Tracking ---
        uint16_t min_val = 4096; 
        uint16_t max_val = 0;    
        // ---------------------------

        for(int i = 0; i < BATCH_SIZE; i++) {
            // Mask to ensure we only look at 12 bits
            uint16_t val = batch.samples[i] & 0xFFF;
            
            // --- Update Min/Max (Raw Data) ---
            if (val < min_val) min_val = val;
            if (val > max_val) max_val = val;
            // ---------------------------

            // --- LAGGED DERIVATIVE CALCULATION ---
            // 1. Get the "Old" value from history
            uint16_t old_val = history[hist_head];
            
            // 2. Overwrite history with current value
            history[hist_head] = val;
            
            // 3. Advance the ring buffer head
            hist_head = (hist_head + 1) % LAG_DEPTH;

            // 4. Calculate Delta against the OLD value
            // We add 2048 to center the result.
            uint16_t delta = (val - old_val + 2048) & 0xFFF;
            // -------------------------------------

            // Update Histogram using DELTA
            if(counts[delta] < 65535) counts[delta]++; 
            if(counts[delta] > max_count) {
                max_count = counts[delta];
            }
        }

        // Calculate Min-Entropy
        float min_entropy = 10.0f - log2f((float)max_count);
        // scale for 8-bit min-entropy - for 10.0f it's 8/10.0
        min_entropy *= (8.0/10.0);
        
        // Calculate Range
        uint16_t dynamic_range = max_val - min_val;

        // --- SQUELCH SAFETY CHECK ---
        // If range is too small, we are likely disconnected/floating.
        // Force entropy to 0.0 to indicate "Unsafe".
        if (dynamic_range < 200) {
            min_entropy = 0.0f;
        }

        // Hashing (SHA-512) - Note: We hash the RAW samples, not the derivative!
        crypto_hash((unsigned char*)batch.samples, sizeof(batch.samples), hash_out_1);
        crypto_hash(hash_out_1, 64, hash_out_2);

        // Output
        printf("H_min: %.4f | R: %4d | Data: \n", min_entropy, dynamic_range);
        print_hex(hash_out_1, 64);
        print_hex(hash_out_2, 64); 
    }
}

// ----------------------------------------------------------------------------
// CORE 0: Hardware Setup & Acquisition
// ----------------------------------------------------------------------------
int main() {
    // 1. Overclock to 250 MHz
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    sleep_ms(10);
    set_sys_clock_khz(TARGET_FREQ_KHZ, true);
    // reinit USB/UART
    stdio_init_all();
    // Uncomment to wait for connection...
    // while (!stdio_usb_connected()) sleep_ms(100);

    // 2. Setup Inter-core Queue
    queue_init(&sample_queue, sizeof(adc_batch_t), 4);

    // 3. Setup PIO Square Wave
    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &square_wave_program);
    square_wave_program_init(pio, sm, offset, PIO_PIN);

    // 4. Setup ADC
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_INPUT);

    // 5. Launch Core 1
    multicore_launch_core1(core1_entry);
    
    printf("Core 0: Generating %d MHz Wave & Sampling ADC.\n", clock_get_hz(clk_sys)/2000000);
    printf("Core 1: Processing Entropy & Hashing.\n");

    // 6. Main Loop
    static adc_batch_t current_batch;
    
    while (true) {
        for(int i = 0; i < BATCH_SIZE; i++) {
            current_batch.samples[i] = adc_read();
            // Jitter the timing slightly to prevent phase locking
            busy_wait_us_32(1 + (adc_read() & 0x03)); 
        }
        queue_add_blocking(&sample_queue, &current_batch);
    }
}
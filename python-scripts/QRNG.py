import machine
import rp2
import _thread
import math
import time
import ubinascii
import uhashlib

# ----------------------------------------------------------------------------
# CONFIGURATION
# ----------------------------------------------------------------------------
TARGET_FREQ = 250_000_000  # 250 MHz System Clock
BATCH_SIZE = 1024          # Samples per batch
LAG_DEPTH = 12             # Compare current sample vs 12 samples ago

# ----------------------------------------------------------------------------
# PIO PROGRAM
# ----------------------------------------------------------------------------
@rp2.asm_pio(set_init=rp2.PIO.OUT_LOW)
def square_wave():
    # The main loop:
    # 1. Set pin high (1 cycle)
    # 2. Set pin low  (1 cycle) + 4 cycles delay
    wrap_target()
    set(pins, 1)
    set(pins, 0) [4] 
    wrap()

# ----------------------------------------------------------------------------
# GLOBALS (Inter-core communication)
# ----------------------------------------------------------------------------
sample_queue = []
queue_lock = _thread.allocate_lock()

# ----------------------------------------------------------------------------
# CORE 1: Processing & Output
# ----------------------------------------------------------------------------
def core1_entry():
    # Ring buffer history
    history = [0] * LAG_DEPTH
    hist_head = 0
    
    print("Core 1: Processing Entropy & Hashing.")
    
    while True:
        batch = None
        with queue_lock:
            if sample_queue:
                batch = sample_queue.pop(0)
        
        if batch is None:
            # No data yet, yield slightly
            time.sleep_ms(1)
            continue
            
        # --- Processing ---
        # Histogram for Min-Entropy (0-4095 for 12-bit delta)
        # Using a list as a sparse array/histogram
        counts = [0] * 4096 
        max_count = 0
        
        min_val = 4096
        max_val = 0
        
        # Prepare bytes for hashing (1024 samples * 2 bytes = 2048 bytes)
        batch_bytes = bytearray(BATCH_SIZE * 2)
        
        for i in range(BATCH_SIZE):
            val = batch[i] & 0xFFF
            
            # Update Min/Max
            if val < min_val: min_val = val
            if val > max_val: max_val = val
            
            # Lagged Derivative Calculation
            old_val = history[hist_head]
            history[hist_head] = val
            hist_head = (hist_head + 1) % LAG_DEPTH
            
            delta = (val - old_val + 2048) & 0xFFF
            
            counts[delta] += 1
            if counts[delta] > max_count:
                max_count = counts[delta]
            
            # Store raw sample in bytearray (Little Endian)
            batch_bytes[2*i] = val & 0xFF
            batch_bytes[2*i+1] = (val >> 8) & 0xFF

        # Calculate Min-Entropy
        min_entropy = 10.0 - math.log2(max_count) if max_count > 0 else 0.0
        
        # Calculate Range
        dynamic_range = max_val - min_val
        
        # Squelch Safety Check
        if dynamic_range < 200:
            min_entropy = 0.0
            
        # Hashing (Using SHA256 as SHA512 isn't available in standard MicroPython)
        h1_ctx = uhashlib.sha256()
        h1_ctx.update(batch_bytes)
        hash_out_1 = h1_ctx.digest()
        
        h2_ctx = uhashlib.sha256()
        h2_ctx.update(hash_out_1)
        hash_out_2 = h2_ctx.digest()
        
        # Output
        print(f"H_min: {min_entropy:.4f} | R: {dynamic_range:4d} | Data: ")
        print(ubinascii.hexlify(hash_out_1).decode())
        print(ubinascii.hexlify(hash_out_2).decode())

# ----------------------------------------------------------------------------
# CORE 0: Hardware Setup & Acquisition
# ----------------------------------------------------------------------------
def main():
    # 1. Overclock to 250 MHz
    machine.freq(TARGET_FREQ)
    
    # 2. Launch Core 1
    _thread.start_new_thread(core1_entry, ())
    
    # 3. Setup PIO Square Wave on Pin 0
    # Note: Pin 0 is set to high drive strength/slew via direct register write if needed, 
    # but standard PIO init is usually sufficient for basic operation.
    # To match C "MAXIMIZE DRIVER PERFORMANCE": PADS_BANK0_GPIO0 (0x4001c004) = 0x31 (Fast Slew, 12mA)
    machine.mem32[0x4001c004] = 0x31 
    
    sm = rp2.StateMachine(0, square_wave, freq=TARGET_FREQ, set_base=machine.Pin(0))
    sm.active(1)
    
    # 4. Setup ADC on Pin 26 (ADC 0)
    adc = machine.ADC(0)
    
    print(f"Core 0: Generating {machine.freq()/1000000} MHz Wave & Sampling ADC.")
    
    # 5. Main Loop
    while True:
        current_batch = []
        for _ in range(BATCH_SIZE):
            # Read ADC (MicroPython returns 16-bit 0-65535)
            val = adc.read_u16()
            current_batch.append(val)
            
            # Jitter: busy_wait_us_32(1 + (adc_read() & 0x03))
            # jitter_read = adc.read_u16()
            # time.sleep_us(1 + (jitter_read & 0x03))
            
        with queue_lock:
            # Simple flow control to prevent OOM if Core 1 is slow
            if len(sample_queue) < 4:
                sample_queue.append(current_batch)

if __name__ == "__main__":
    main()
    
"""
Example Output:

H_min: 8.0000 | R: 4085 | Data: 
842de02eb46a77810ee1f12867654189e2ae93e5f4b6c0063d6b97ef99f63d49
1486b7fa7c1042eaba91be1799287c671ffde3ec8da91f313fee71359469b20b
H_min: 8.0000 | R: 4085 | Data: 
8c66a757ca5c19331c8fa8c76414b0e158c15b459ad626de41bfc65677b67629
7421bf931a3734f7181f507f729bdc5e748d596ce78249480e97089908d9d5d0
H_min: 7.6781 | R: 4084 | Data: 
19aca923ff67b3eb41dd09adf74ae1ebb82ca4bc34d8b7d0d8089e93330e43b3
9711ac95629509259d2ed54fe3d35225632229ec206fcb9766b2d11aa4e59c1f
H_min: 7.4150 | R: 4079 | Data: 
83b5f9a5dfb404611f4895fd71b2394809211055ce59fcbefb7004b666d3254b
1e5bf02464b1212de5e96352f032d4a876b0d3b27032fb3c404fedbe373a90eb
H_min: 8.0000 | R: 4085 | Data: 
3d7bf71e9cb36ce623301a9f84724e2cc9369d19b69978d87652de3139372024
a6b99a5e707025ca99b64abec72f6859199604227f52c7b8b7ff5091cf25fe81
H_min: 7.4150 | R: 4083 | Data: 
a6017bef001deff0f585af06113411341100a66a3e0438e865a0c2e4df13cfba
be1eb5e0098012e5e9fbb316320b1f99418ff54b28b89d3fc76fcedea2e2b930
H_min: 7.6781 | R: 4083 | Data: 
f6642e321cff06275e28f6788d4e723e70a59ef94c7e0fa615359dcdda9981c1
ca5811832adff804d2caca39e0bf1054af95ae260708e98318a05fc92b867623
H_min: 7.6781 | R: 4082 | Data: 
e3a0cbaec487de4fd85fc172743a6ebcaec8411866fb22d342f92a924d260b6a
ca32af1cafbbafe175bc0ef189d959ecfe9782cef08cee2151188eac3dd0c07c
H_min: 8.0000 | R: 4083 | Data: 
41e997935135564036925dc54286c9d0f22bc31acfe07b389ca43d929cdcac00
4e650b21666ecaf88e4262f52646c24e2c98d10e98b1e7554c177129168b2de4
"""
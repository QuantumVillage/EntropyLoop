# Dual-Path EntropyLoop

**Branch: dual-path | Built on top of QuantumVillage/EntropyLoop**
**Contributors: E (cryptocrack4011), Mark Carney (QuantumVillage), Victoria Kumaran**

## What This Is

The original EntropyLoop uses a single detection path — one receiver, one ADC channel,
one measurement. This branch adds **balanced differential detection**: a second receiver
on the other output port of the MZI, and a differential ADC measuring the difference
between both ports simultaneously.

This separates the quantum optical signal from classical electrical noise by design.
Common-mode noise (power supply, EMI, temperature drift) appears identically on both
channels and cancels in the subtraction. What remains is the quantum interference signal.

## Architecture Comparison

| | EntropyLoop (original) | Dual-Path |
|---|---|---|
| Receivers | 1 (TRX-B) | 2 (TRX-B + TRX-C) |
| Coupler | 1×2 splitter | 2×2 balanced coupler |
| ADC channels | 1 (internal 12-bit) | 2 (internal + external differential) |
| External ADC | None | ADS1115 (Build 2.1) → AD7606 (Build 3.x) |
| Common-mode rejection | None | Hardware differential subtraction |
| Drift compensation | None | Software DC blocker (Build 3.1+) |
| Debiasing | None | Von Neumann (Build 3.1+) |
| H_min (internal) | ~6.0–6.5 bits/byte | 5.1–5.5 bits/byte (Build 2.1) |
| H_min (differential) | — | 3.7–4.4 (Build 2.1) → **6.2–6.9 sustained (Build 3.2)** |

## Build History

| Build | ADC | H_min DIFF | H_min INT | Status |
|-------|-----|-----------|-----------|--------|
| 2.1 | ADS1115 16-bit, 860 SPS, PIO I2C | 3.7–4.4 | 5.1–5.5 | Superseded |
| 3.0 | AD7606 16-bit, 200k SPS, SPI | ~6.45 peak | — | Drift issue |
| 3.1 | AD7606 + DC blocker + oversampling | stable | 5.4–5.9 | Production |
| 3.2 | AD7606 + busy-race fix | **6.2–6.9 sustained** | 5.4–5.9 | **Current** |

## Additional Hardware Required

Beyond the standard EntropyLoop BOM:

| Part | Purpose |
|------|---------|
| 1× additional SFP transceiver (TRX-C) | Second receiver — destructive port |
| 1× 2×2 fiber coupler (50/50) | Replace one 1×2 splitter for balanced detection |
| ADS1115 breakout (Build 2.1) OR AD7606 board (Build 3.x) | External differential ADC |
| 4× 10kΩ resistors | Bias circuits for both receivers |
| 2× 10nF capacitors | AC coupling on both channels |

### Build 3.x additional wiring (AD7606)
| Pico 2 GPIO | AD7606 Pin | Notes |
|---|---|---|
| GP16 | D0 (MISO) | Via 10kΩ/20kΩ voltage divider — AD7606 is 5V |
| GP17 | CS | Direct |
| GP18 | RD (SCLK) | Direct |
| GP20 | CB (CONVST) | Conversion trigger |
| GP21 | BUSY | Via 10kΩ/20kΩ voltage divider |
| GP10 | OS0 | Oversampling control |
| GP11 | OS1 | Oversampling control |
| GP12 | OS2 | Oversampling control |
| GP13 | RANGE | ±5V / ±10V select |

## Firmware

- `firmware/main_build2.1.c` — PIO I2C + ADS1115 dual channel, 250MHz
- `firmware/main_build3.1a.c` — AD7606 + DC blocker + Von Neumann, 250MHz
- `firmware/CMakeLists.txt` — Pico SDK build config

## Releases

- `releases/fiberranger_build2.1_functional_baseline.uf2` — Build 2.1 confirmed working (154K)
- `releases/fiberranger_v3.2.uf2` — Build 3.2 current production (89K), 6.2–6.9 bits/byte sustained

## Live Results

Build 3.2 running continuously on bench hardware:
- `diff_h`: 6.2–6.9 bits/byte sustained over 4+ days
- `int_h`: 5.4–5.9 bits/byte
- RCT/APT: PASS, zero timeouts
- vn_bias: ~0 (perfectly balanced)
- Hardware oversampling: 2× on AD7606

## Monitoring Stack

InfluxDB + Grafana dashboard — see `monitoring/` directory.
Live dashboard panels: H_min over time, VN bias, NIST health tests (RCT/APT),
Von Neumann throughput, raw channel stats (CH1/CH2/DIFF min/max/spread).

## Relationship to EntropyLoop

This is a direct extension of Mark Carney's EntropyLoop design. The original
single-path firmware, PIO square wave driver, lagged derivative entropy estimator,
and SHA-512 pipeline are preserved unchanged. The dual-path adds a second measurement
channel on top without modifying the core architecture.



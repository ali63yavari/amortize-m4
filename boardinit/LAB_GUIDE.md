# AMORTIZE Lab Guide: Hardware Benchmarking, Formal Verification, and TVLA

This guide gives complete, ordered instructions for the three remaining
experimental steps of the manuscript. Follow them in order: Step 2 produces
the cycle counts for Table II, Step 3 produces the verification artifacts
behind Theorem 1, and Step 4 produces Fig. 3. Budget estimate: about USD 400
total. Time estimate: 4 to 8 weeks part time.

---

## STEP 2: Real Cortex-M4 Cycle Counts

### 2.1 What to buy

1. STM32F407G-DISC1 Discovery board (about USD 25 to 30; available from
   element14, RS Components, Mouser, or Cytron in Malaysia).
2. One USB-A to mini-USB cable (powers the board and connects ST-LINK).
3. One USB-to-TTL serial adapter, 3.3 V logic (CP2102 or FT232RL, about
   USD 3), plus three female-to-female jumper wires, to read benchmark
   output over UART.

### 2.2 Software setup (Ubuntu 22.04+ native or Windows WSL2)

```
sudo apt update
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi \
                 libnewlib-arm-none-eabi openocd stlink-tools \
                 python3-pip git make
pip3 install pyserial
```

Confirm the toolchain: `arm-none-eabi-gcc --version` should print a version.
Plug in the board over mini-USB and confirm detection: `st-info --probe`
should report one ST-LINK device. On WSL2 you need usbipd-win to forward
the USB device into Linux; follow the usbipd README.

### 2.3 Two integration paths

Path A (recommended first, 1 to 2 days): STM32CubeIDE standalone project.
Fastest route to numbers for the conversion layer, which is what Table II
and Section V-D need.

Path B (afterwards, 1 to 2 weeks): full pqm4 integration. Needed only when
you want end-to-end masked decapsulation cycles, which requires masking the
rest of the scheme too. Do Path A first; it derisks everything.

### 2.4 Path A in detail

1. Install STM32CubeIDE (free, st.com). Create a new STM32 project for
   target STM32F407VGTx.
2. In the CubeMX device configuration view:
   a. System Core > RCC: set HSE to Crystal/Ceramic Resonator.
   b. Clock tab: set HCLK to 24 MHz (the pqm4 benchmarking convention;
      it eliminates flash wait-state variance). CubeMX solves the PLL
      settings for you when you type 24 into the HCLK box.
   c. Security > RNG: Activate. This is the hardware TRNG.
   d. Connectivity > USART2: Mode Asynchronous, 115200 baud, 8N1.
      Pins PA2 (TX) and PA3 (RX) appear automatically.
3. Generate code. Copy `gadgets.c`, `gadgets.h`, and `bench_m4.c` from
   the amortize_m4 folder into `Core/Src` and `Core/Inc`.
4. Wire the pieces:
   a. In bench_m4.c, the `#ifdef CORTEX_M4` block already contains the
      RNG register code and the DWT cycle counter. Define `CORTEX_M4`
      in Project Properties > C/C++ Build > Settings > Preprocessor.
   b. Replace `uart_puts` and `uart_putu32` with wrappers around
      `HAL_UART_Transmit`, or retarget printf by implementing
      `int __io_putchar(int ch)`.
   c. Delete the generated `main.c` main loop or call our benchmark
      main from it (rename ours to `bench_main` to avoid a clash).
5. In the linker script (`STM32F407VGTX_FLASH.ld`), set
   `_Min_Stack_Size = 0x4000;` (16 KiB). The tree conversion is
   recursive and the default 1 KiB stack will overflow silently.
6. Disable interrupts around the timed region (`__disable_irq()` /
   `__enable_irq()`) so SysTick does not pollute the counts.
7. Build, flash (the IDE uses the onboard ST-LINK), connect the serial
   adapter (board PA2 to adapter RX, PA3 to TX, GND to GND), and read:
   `screen /dev/ttyUSB0 115200` (or PuTTY on Windows).
8. You will see one line per order with `seq_cycles_per_poly` and
   `tree_cycles_per_poly`. Each is for 256 coefficients (8 batches).

### 2.5 Measurement protocol (do not skip)

1. Run each order 11 times; record the median. The first run is warm-up
   and is discarded. RNG stalls cause run-to-run variance; the median
   absorbs it.
2. Convert: cycles_per_coefficient = median_cycles / 256.
3. Sanity checks before trusting any number:
   a. The d = 1 tree and sequential numbers should be within a few
      percent of each other (the algorithms coincide at one addition).
   b. Ratios across orders should roughly track the Python operation
      counts in results_v3.csv (constants differ; exponents should not).
   c. If tree numbers explode or the board hard-faults, it is the stack:
      re-check item 2.4.5.
4. Fill the `decaps_cycles` column of results_v3.csv with the per-order,
   per-variant cycles per coefficient and send the CSV back for the
   manuscript update.

### 2.6 Path B notes (pqm4, later)

Clone with submodules: `git clone --recursive https://github.com/mupq/pqm4`.
Read its README for the current build and benchmark invocation; conventions
change between releases, so follow the README rather than memory. Our code
slots in as a protected variant under `crypto_kem/ml-kem-768/`; the masked
decapsulation around it (NTT, sampler, Keccak masking) is its own project
and is exactly what the paper's "end-to-end cycles pending" caveat covers.

---

## STEP 3: Formal Verification of the Gadgets

### 3.1 What is being proven, per gadget

| Gadget            | Property to check | Orders        |
|-------------------|-------------------|---------------|
| isw_and           | PINI (and SNI)    | d = 1..highest feasible |
| refresh_sni       | SNI               | same          |
| linear_refresh    | NI / uniformity   | same          |
| inject_uniform    | uniform output sharing | argue by hand: output is a fresh uniform sharing by construction |
| ks_add            | follows by composition of verified isw_and and XOR |
| tree conversion   | does NOT follow from plain PINI composition; see 3.4 |

### 3.2 Tool installation

Primary tool: IronMask (CryptoExperts), a command-line checker for NI, SNI,
and PINI of masked gadgets.

```
git clone https://github.com/CryptoExperts/IronMask
cd IronMask && make
```

Secondary tool (cross-check): maskVerif (Barthe et al.). Both repositories
ship an `examples/` directory; the input syntax in those examples is the
authoritative reference. Encode our gadgets by editing the closest shipped
example (ISW multiplication is always included) rather than writing from
scratch.

### 3.3 Process

1. Encode isw_and exactly as implemented in gadgets.c: same randoms, same
   order of XORs. The order of operations matters for the property; do not
   encode the textbook version if the code differs.
2. Run the PINI check at d = 1, 2, 3, then as high as compute allows.
   Exact verification is combinatorial; d = 4 or 5 for multiplication is
   a realistic ceiling on a workstation. State the verified range in the
   paper and claim higher orders only via the composition theorem.
3. Repeat for refresh_sni (SNI check) and linear_refresh.
4. Save every tool transcript (command line plus output) into a
   `verification/` folder in the repository. These transcripts are the
   artifact reviewers will ask for, and TCHES artifact evaluation will
   re-run them.

### 3.4 The tree gadget (the hard part, plan for help)

The tree conversion computes on sharings of reduced size inside subtrees,
which plain PINI composition does not cover. The security argument must
follow the proof structure of Coron, Grossschaedl, and Vadnala (CHES 2014):
the key lemma is that a sum of a proper subset of arithmetic shares is
uniform and independent of the secret, so probes inside a subtree of size
m cost the adversary budget against an m-share computation. Concretely:

1. Read the CGV14 security proof in full and map each step of their
   conversion onto our tree_rec, documenting any divergence (our linear
   refresh at merges versus their refresh placement is the main one).
2. Write the reduction as a lemma in the paper's appendix.
3. Have it checked by someone who has written probing-security proofs
   before (your supervisor, or reach out to a masking researcher; this
   community responds well to concrete questions with code attached).
4. Until that lemma is verified, the paper's existing caveat sentence in
   Section V-D must remain. Do not delete it to make the paper look
   stronger; reviewers in this area will reconstruct the gap in minutes.

---

## STEP 4: TVLA Leakage Assessment

### 4.1 What to buy

Recommended: ChipWhisperer-Lite 32-bit starter kit plus the CW308 UFO
baseboard with the CW308T-STM32F4 target (NewAE Technology; total about
USD 350 to 450). This gives synchronized capture, a clean shunt
measurement, and a maintained Python stack, and the STM32F4 target matches
the manuscript's platform claim.

Budget alternative (harder, only if funds are tight): a 100 MS/s+ USB
oscilloscope and a 10 ohm shunt in the Discovery board's IDD jumper, with
a GPIO trigger. Expect weeks of alignment pain; the ChipWhisperer route is
worth the money.

### 4.2 Software

```
pip3 install chipwhisperer jupyter numpy scipy matplotlib
```

Run the ChipWhisperer "connect and capture" tutorial notebook first and
confirm you can capture traces from the stock firmware before touching
our code.

### 4.3 Port the firmware to SimpleSerial

1. In the ChipWhisperer firmware tree, copy `simpleserial-base` to
   `simpleserial-amortize`, drop in gadgets.c/h.
2. Implement one command handler: receive 16 bytes (a seed), expand it
   to the 32-coefficient arithmetic sharing on-device, raise the trigger
   pin, run `a2b_tree_batch32` once, lower the trigger, return one byte.
3. IMPORTANT: gadget randomness must come from the target's TRNG or an
   onboard PRG reseeded per trace, never from the host, or the test is
   meaningless.
4. Build with `make PLATFORM=CW308_STM32F4 CRYPTO_TARGET=NONE` and flash
   through the ChipWhisperer notebook.

### 4.4 Fixed-versus-random capture design

1. Two input classes: FIXED (one constant coefficient vector, chosen once)
   and RANDOM (fresh uniform vector each trace).
2. Interleave classes randomly per trace (coin flip per capture), never
   in blocks; block ordering lets drift masquerade as leakage.
3. Capture at least 100,000 traces per class per order. Start with
   d = 1 and d = 2; higher orders need more traces and longer runs.
4. Controls, both mandatory for credibility:
   a. Negative control: compile with masking randomness forced to zero
      (rand32 returns 0). The t-test MUST fail loudly. If it does not,
      your setup cannot see leakage and a pass means nothing.
   b. Positive control: with masking on, the test should pass at the
      implemented order.

### 4.5 Computing the statistic

First-order Welch t-test per sample index:

    t[j] = (mean_F[j] - mean_R[j]) /
           sqrt(var_F[j]/n_F + var_R[j]/n_R)

For order d > 1, follow the Schneider-Moradi methodology cited in the
paper: center each trace by its class mean and raise to the d-th power
(standardized central moments), then apply the same t-test to the
preprocessed traces. Numpy sketch:

```python
import numpy as np
def welch_t(F, R):           # F, R: (n_traces, n_samples)
    mF, mR = F.mean(0), R.mean(0)
    vF, vR = F.var(0, ddof=1), R.var(0, ddof=1)
    return (mF - mR) / np.sqrt(vF/len(F) + vR/len(R))
```

### 4.6 Reporting

1. Pass criterion: |t| < 4.5 at every sample for the order under test,
   with the trace count stated.
2. Export per order: `tvla_order{d}.csv` with columns `sample,t_stat`,
   then run the existing plot_figures.py to generate Fig. 3.
3. Report the negative control plot alongside the pass plots in the
   paper or its artifact; it is the evidence that the pass is real.

---

## Order of operations summary

1. Step 2 Path A (cycle counts) unblocks Table II: do first.
2. Step 3 runs in parallel on your workstation; the tree lemma (3.4)
   is the item to start earliest because it may need a collaborator.
3. Step 4 last; it reuses the Step 2 firmware work and is the longest.

When you have (a) the filled CSV from Step 2, (b) verification
transcripts from Step 3, or (c) tvla_order CSVs from Step 4, send them
back and the manuscript gets updated with measured, defensible values.

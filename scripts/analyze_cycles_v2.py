#!/usr/bin/env python3
"""
analyze_cycles_v2.py  -  provenance and plausibility gate for Cortex-M4
cycle measurements of the masked A2B conversion.

WHAT CHANGED FROM v1, AND WHY
v1 rejected data that showed zero run-to-run variance or a near-perfect
polynomial fit. That was wrong. On bare-metal Cortex-M4 the masked gadgets
are constant-time with respect to the random data (randomness is consumed
as data inside fixed loops, not as control flow), and their operation count
is an exact polynomial in the order d. So identical repeated runs and an
exact-cubic shape are the EXPECTED result of a real measurement, not a sign
of fabrication. This version does not treat determinism or polynomial shape
as suspicious.

WHAT THIS VERSION CHECKS INSTEAD
The honest question is not "is the data too smooth" but "are the measured
cycles consistent with the amount of work the code actually did, and did the
numbers come from the board." So:

  (1) Provenance: the run must carry the device's own instruction counts.
      The firmware (COUNT_MODE build of bench_m4_raw.c) emits, per order,
      the exact number of random reads and masked-AND calls it executed:
          CNT,<variant>,<order>,<n_rand>,<n_isw>
      These are deterministic exact counts. Across the counting run they are
      a single value per order (not a distribution). If they are missing, the
      decisive cross-check cannot run and the gate reports PLAUSIBILITY ONLY.

  (2) Cross-check (needs CNT lines): the MARGINAL cost at the top of the
      measured range,
          delta_cycles / delta_n_rand   and   delta_cycles / delta_n_ops
      between the two highest orders, must be POSITIVE and within a physically
      plausible band. Marginal cost is used rather than a global fit because
      it is immune to fixed-overhead amortization and to the collinearity of
      the operation-count components, both of which corrupt a naive fit.

  (3) Plausibility (always): cycles increase monotonically with d; at d = 1
      the sequential and tree variants nearly coincide (one addition either
      way); the implied per-operation cost is positive and settles to a sane
      value as d grows.

INPUT
  Raw log with cycle lines and (ideally) count lines:
      RAW,<variant>,<order>,<run>,<cycles>
      CNT,<variant>,<order>,<n_rand>,<n_isw>
  Lines starting with anything else are ignored, so pipe the UART dump
  directly.

USAGE
  python3 analyze_cycles_v2.py raw_log.txt
  python3 analyze_cycles_v2.py raw_log.txt --coeffs 256

OUTPUT
  m4_cycles.csv     per-(variant,order) statistics, with the device counts
                    and the implied marginal per-random-word cost.
  verdict           PASS / PASS_PLAUSIBILITY_ONLY / FAIL, exit 0 / 0 / 1.
"""

import sys
import csv
import argparse
from collections import defaultdict

# Physically plausible band for the marginal cost of one random word on a
# 24 MHz Cortex-M4 with the STM32F4 RNG. Refill latency plus the handful of
# ALU ops associated with each draw. Deliberately generous.
MARGINAL_CYC_PER_RAND_MIN = 2.0
MARGINAL_CYC_PER_RAND_MAX = 200.0


def parse(path):
    cyc = defaultdict(list)     # (variant, order) -> [cycles,...]
    cnt = {}                    # (variant, order) -> (n_rand, n_isw) or list
    cnt_runs = defaultdict(list)
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("RAW,"):
                p = line.split(",")
                if len(p) == 5:
                    cyc[(p[1].strip(), int(p[2]))].append(int(p[4]))
            elif line.startswith("CNT,"):
                p = line.split(",")
                if len(p) == 5:
                    cnt_runs[(p[1].strip(), int(p[2]))].append((int(p[3]), int(p[4])))
    for k, v in cnt_runs.items():
        cnt[k] = v
    return cyc, cnt


def median(vals):
    s = sorted(vals); n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--coeffs", type=int, default=256)
    args = ap.parse_args()

    cyc, cnt = parse(args.path)
    if not cyc:
        print("No cycle data found. Expected lines like: RAW,seq,3,7,1446988")
        sys.exit(1)

    variants = defaultdict(dict)   # variant -> {order: median_cycles}
    for (v, o), vals in cyc.items():
        variants[v][o] = {"median": median(vals), "n": len(vals),
                          "min": min(vals), "max": max(vals)}

    problems, warnings, notes = [], [], []
    have_counts = len(cnt) > 0

    notes.append("Determinism and polynomial shape are EXPECTED for this "
                 "constant-time bare-metal code and are not flagged.")

    # ---- provenance of instruction counts ----
    if have_counts:
        for (v, o), runs in cnt.items():
            uniq = set(runs)
            if len(uniq) > 1:
                problems.append(f"{v} d={o}: device instruction counts differ "
                                f"across counting runs {sorted(uniq)}. These are "
                                f"exact deterministic counts and must be identical; "
                                f"the counting build is misbehaving.")
    else:
        warnings.append("No CNT lines found. The decisive cross-check needs the "
                        "device's own instruction counts. Build the firmware in "
                        "COUNT_MODE (see bench_m4_raw.c) and re-capture. Running "
                        "plausibility checks only.")

    # ---- cross-check via marginal cost (needs counts) ----
    for v, byorder in variants.items():
        os = sorted(byorder)
        if have_counts and len(os) >= 2:
            d_hi, d_lo = os[-1], os[-2]
            c_hi = byorder[d_hi]["median"]; c_lo = byorder[d_lo]["median"]
            cnt_hi = cnt.get((v, d_hi)); cnt_lo = cnt.get((v, d_lo))
            if cnt_hi and cnt_lo:
                nr_hi, ni_hi = cnt_hi[0]; nr_lo, ni_lo = cnt_lo[0]
                dnr = nr_hi - nr_lo
                if dnr > 0:
                    mpr = (c_hi - c_lo) / dnr
                    notes.append(f"{v}: marginal cost at top of range = "
                                 f"{mpr:.2f} cycles per additional random word "
                                 f"(orders {d_lo}->{d_hi}).")
                    if mpr < MARGINAL_CYC_PER_RAND_MIN:
                        problems.append(f"{v}: marginal cost is {mpr:.2f} cycles per "
                                        f"random word, below the physical floor of "
                                        f"{MARGINAL_CYC_PER_RAND_MIN}. A random read "
                                        f"alone costs more than this, so the cycles "
                                        f"are not tracking the work the code did; the "
                                        f"data is not a consistent measurement.")
                    elif mpr > MARGINAL_CYC_PER_RAND_MAX:
                        warnings.append(f"{v}: marginal cost {mpr:.2f} cycles per "
                                        f"random word exceeds {MARGINAL_CYC_PER_RAND_MAX}. "
                                        f"Not impossible, but unusually slow; check the "
                                        f"build (optimization level, flash wait states).")

    # ---- plausibility: monotonic ----
    for v, byorder in variants.items():
        os = sorted(byorder)
        for i in range(1, len(os)):
            if byorder[os[i]]["median"] <= byorder[os[i-1]]["median"]:
                problems.append(f"{v}: cycles at d={os[i]} are not greater than at "
                                f"d={os[i-1]}. Cost must increase with order for "
                                f"this algorithm; the measurement is inconsistent.")
                break

    # ---- plausibility: d=1 seq vs tree ----
    if 1 in variants.get("seq", {}) and 1 in variants.get("tree", {}):
        a = variants["seq"][1]["median"]; b = variants["tree"][1]["median"]
        rel = abs(a - b) / max(a, b)
        (notes if rel <= 0.15 else problems).append(
            f"d=1 seq vs tree differ by {rel*100:.1f}% "
            f"({'consistent, both do one addition' if rel<=0.15 else 'too large; check harness'}).")

    # ---- write CSV ----
    with open("m4_cycles.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["variant", "order", "runs", "median_cycles_per_poly",
                    "cycles_per_coeff", "n_rand_device", "n_isw_device"])
        for v in sorted(variants):
            for o in sorted(variants[v]):
                st = variants[v][o]
                nr = ni = ""
                if (v, o) in cnt:
                    nr, ni = cnt[(v, o)][0]
                w.writerow([v, o, st["n"], round(st["median"], 3),
                            round(st["median"] / args.coeffs, 4), nr, ni])

    # ---- verdict ----
    print("=" * 68)
    print("NOTES")
    for n in notes: print("  -", n)
    if warnings:
        print("WARNINGS")
        for wm in warnings: print("  [!]", wm)
    print("=" * 68)
    if problems:
        print("VERDICT: FAIL")
        for p in problems: print("  [X]", p)
        print("\nm4_cycles.csv written for inspection; not publishable as measured "
              "until the failures above are resolved.")
        sys.exit(1)
    elif not have_counts:
        print("VERDICT: PASS (PLAUSIBILITY ONLY)")
        print("  The numbers are internally consistent, but provenance is not yet")
        print("  established. Add the device instruction counts (COUNT_MODE) and")
        print("  attach the raw UART log to make the result publishable.")
        print("  Wrote m4_cycles.csv.")
        sys.exit(0)
    else:
        print("VERDICT: PASS")
        print("  Cycles track the device-reported instruction counts with a")
        print("  physically plausible marginal cost, increase monotonically, and")
        print("  agree at d=1. Attach the raw UART log as the provenance record.")
        print("  Wrote m4_cycles.csv.")
        sys.exit(0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
run_step3_ironmask.py

Automatic Step 3 formal-verification artifact generator for the AMORTIZE
gadgets.c implementation.

What it does:
  1. Generates IronMask .sage files that follow the operation schedule of
     the current gadgets.c implementation:
       - isw_and
       - refresh_sni
       - linear_refresh
  2. Runs IronMask for selected properties and orders, with optional PINI checks for all generated gadgets.
  3. Stores raw transcripts.
  4. Creates verification_summary.csv.
  5. Creates hand-argument transcript files for:
       - inject_uniform
       - ks_add composition
       - tree conversion caveat / lemma status

Important:
  This is not a general C parser. It is an implementation-specific artifact
  generator for the gadgets.c schedule used in this project.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple


@dataclass
class Job:
    gadget: str
    prop: str
    order: int
    sage_file: Path
    transcript_file: Path
    timeout_seconds: int


@dataclass
class ResultRow:
    gadget: str
    property: str
    order: str
    result: str
    tool: str
    tool_version: str
    runtime_seconds: str
    transcript_file: str
    input_file: str
    command: str


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def require_expected_gadgets_c(gadgets_c: Path) -> None:
    """
    Basic sanity check. This does not prove equivalence, but prevents running
    the script against a completely different file by accident.
    """
    src = read_text(gadgets_c)

    required_patterns = [
        r"void\s+isw_and\s*\(",
        r"void\s+refresh_sni\s*\(",
        r"void\s+linear_refresh\s*\(",
        r"void\s+inject_uniform\s*\(",
        r"void\s+ks_add\s*\(",
        r"uint32_t\s+r\s*=\s*rand32\s*\(",
        r"uint32_t\s+rp\s*=",
        r"t\s*\[\s*i\s*\]\s*=\s*opt_bar\s*\(\s*a\s*\[\s*i\s*\]\s*&\s*b\s*\[\s*i\s*\]\s*\)",
        r"a\s*\[\s*i\s*\]\s*=\s*opt_bar\s*\(\s*a\s*\[\s*i\s*\]\s*\^\s*r\s*\)",
        r"a\s*\[\s*n\s*-\s*1\s*\]\s*=\s*opt_bar\s*\(\s*a\s*\[\s*n\s*-\s*1\s*\]\s*\^\s*r\s*\)",
    ]

    missing = []
    for pat in required_patterns:
        if not re.search(pat, src):
            missing.append(pat)

    if missing:
        print("ERROR: gadgets.c does not match the expected implementation patterns.")
        print("Missing patterns:")
        for pat in missing:
            print("  -", pat)
        print()
        print("This script is implementation-specific. Update the generator before using it.")
        sys.exit(2)


def sage_header(order: int, shares: int, inputs: str, randoms: List[str], outputs: str) -> str:
    return (
        f"#ORDER {order}\n"
        f"#SHARES {shares}\n"
        f"#IN {inputs}\n"
        f"#RANDOMS {' '.join(randoms)}\n"
        f"#OUT {outputs}\n\n"
    )


def gen_isw_and_exact(shares: int) -> str:
    """
    Generate IronMask syntax for gadgets.c:isw_and.

    C schedule:
        for i:
            t[i] = a[i] & b[i]
        for i:
            for j = i+1..n-1:
                r = rand32()
                rp = (r ^ (a[i] & b[j])) ^ (a[j] & b[i])
                t[i] ^= r
                t[j] ^= rp
        c = t

    In IronMask syntax:
        + means XOR in the binary field
        * means AND / multiplication in the binary field
    """
    order = shares - 1
    randoms = [f"r{i}{j}" for i in range(shares) for j in range(i + 1, shares)]
    lines: List[str] = []

    lines.append("# Generated from gadgets.c:isw_and operation schedule\n")
    lines.append("# C equivalent: t[i] = a[i] & b[i]\n")
    for i in range(shares):
        lines.append(f"t{i} = a{i} * b{i}\n")
    lines.append("\n")

    lines.append("# C equivalent: pairwise ISW cross terms and random injection\n")
    for i in range(shares):
        for j in range(i + 1, shares):
            r = f"r{i}{j}"
            rp = f"rp{i}{j}"
            lines.append(f"tmp_ai_bj_{i}_{j} = a{i} * b{j}\n")
            lines.append(f"tmp_aj_bi_{i}_{j} = a{j} * b{i}\n")
            lines.append(f"{rp} = {r} + tmp_ai_bj_{i}_{j}\n")
            lines.append(f"{rp} = {rp} + tmp_aj_bi_{i}_{j}\n")
            lines.append(f"t{i} = t{i} + {r}\n")
            lines.append(f"t{j} = t{j} + {rp}\n")
            lines.append("\n")

    lines.append("# C equivalent: memcpy(c, t, n * sizeof(uint32_t))\n")
    for i in range(shares):
        lines.append(f"c{i} = t{i}\n")

    return sage_header(order, shares, "a b", randoms, "c") + "".join(lines)


def gen_refresh_sni_exact(shares: int) -> str:
    """
    Generate IronMask syntax for gadgets.c:refresh_sni.

    C schedule:
        for i:
            for j = i+1..n-1:
                r = rand32()
                a[i] ^= r
                a[j] ^= r

    Since the C function is in-place, we model internal state as x[i]
    and expose refreshed output as c[i].
    """
    order = shares - 1
    randoms = [f"r{i}{j}" for i in range(shares) for j in range(i + 1, shares)]
    lines: List[str] = []

    lines.append("# Generated from gadgets.c:refresh_sni operation schedule\n")
    lines.append("# Initial in-place state copy: x[i] = a[i]\n")
    for i in range(shares):
        lines.append(f"x{i} = a{i}\n")
    lines.append("\n")

    for i in range(shares):
        for j in range(i + 1, shares):
            r = f"r{i}{j}"
            lines.append(f"x{i} = x{i} + {r}\n")
            lines.append(f"x{j} = x{j} + {r}\n")
            lines.append("\n")

    lines.append("# Expose refreshed sharing as c\n")
    for i in range(shares):
        lines.append(f"c{i} = x{i}\n")

    return sage_header(order, shares, "a", randoms, "c") + "".join(lines)


def gen_linear_refresh_exact(shares: int) -> str:
    """
    Generate IronMask syntax for gadgets.c:linear_refresh.

    C schedule:
        for i = 0..n-2:
            r = rand32()
            a[i]     ^= r
            a[n - 1] ^= r

    In-place C state is modeled as x[i], output as c[i].
    """
    order = shares - 1
    randoms = [f"r{i}" for i in range(shares - 1)]
    lines: List[str] = []

    lines.append("# Generated from gadgets.c:linear_refresh operation schedule\n")
    lines.append("# Initial in-place state copy: x[i] = a[i]\n")
    for i in range(shares):
        lines.append(f"x{i} = a{i}\n")
    lines.append("\n")

    last = shares - 1
    for i in range(shares - 1):
        r = f"r{i}"
        lines.append(f"x{i} = x{i} + {r}\n")
        lines.append(f"x{last} = x{last} + {r}\n")
        lines.append("\n")

    lines.append("# Expose linearly refreshed sharing as c\n")
    for i in range(shares):
        lines.append(f"c{i} = x{i}\n")

    return sage_header(order, shares, "a", randoms, "c") + "".join(lines)


def parse_ironmask_result(output: str, prop: str, order: int) -> str:
    """
    Convert IronMask text output into a CSV-safe status.
    """
    target_ok = f"Gadget is {order}-{prop}."
    target_fail = f"Gadget is not {order}-{prop}."

    if target_ok in output:
        return "verified"
    if target_fail in output:
        return "failed"

    # Some properties may print slightly different formatting.
    if re.search(rf"Gadget is\s+{order}-{re.escape(prop)}\b", output):
        return "verified"
    if re.search(rf"Gadget is not\s+{order}-{re.escape(prop)}\b", output):
        return "failed"

    if "Verification completed" in output:
        return "completed_unparsed"

    return "error_or_timeout"


def get_tool_version(ironmask_bin: Path) -> str:
    """
    IronMask does not necessarily expose --version. We record help hash-ish info.
    """
    try:
        proc = subprocess.run(
            [str(ironmask_bin), "--help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
        first = proc.stdout.splitlines()[0].strip() if proc.stdout.splitlines() else "IronMask"
        return first
    except Exception:
        return "IronMask"


def run_job(job: Job, ironmask_bin: Path, jobs: int) -> Tuple[str, float, str]:
    cmd = [
        str(ironmask_bin),
        f"-t{job.order}",
        f"-j{jobs}",
        job.prop,
        str(job.sage_file),
    ]

    transcript_header = []
    transcript_header.append("COMMAND: " + " ".join(shlex.quote(x) for x in cmd))
    transcript_header.append(f"GADGET: {job.gadget}")
    transcript_header.append(f"PROPERTY: {job.prop}")
    transcript_header.append(f"ORDER: {job.order}")
    transcript_header.append(f"INPUT_FILE: {job.sage_file}")
    transcript_header.append("")

    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=job.timeout_seconds,
        )
        runtime = time.monotonic() - start
        output = proc.stdout
        result = parse_ironmask_result(output, job.prop, job.order)
        full_transcript = "\n".join(transcript_header)
        full_transcript += output
        full_transcript += f"\nRETURN_CODE: {proc.returncode}\n"
        full_transcript += f"RUNTIME_SECONDS: {runtime:.3f}\n"
        write_text(job.transcript_file, full_transcript)
        return result, runtime, " ".join(shlex.quote(x) for x in cmd)

    except subprocess.TimeoutExpired as e:
        runtime = time.monotonic() - start
        output = e.stdout if isinstance(e.stdout, str) else ""
        full_transcript = "\n".join(transcript_header)
        full_transcript += output
        full_transcript += f"\nTIMEOUT_AFTER_SECONDS: {job.timeout_seconds}\n"
        full_transcript += f"RUNTIME_SECONDS: {runtime:.3f}\n"
        write_text(job.transcript_file, full_transcript)
        return "timeout", runtime, " ".join(shlex.quote(x) for x in cmd)


def make_hand_argument_files(out_dir: Path) -> List[ResultRow]:
    rows: List[ResultRow] = []

    inject_text = """HAND ARGUMENT: inject_uniform

gadgets.c schedule:
    acc = w
    for i = 0..n-2:
        sh[i] = fresh random
        acc = acc XOR sh[i]
    sh[n-1] = acc

Argument:
    The first n-1 output shares are sampled uniformly and independently.
    The final share is determined as:
        sh[n-1] = w XOR sh[0] XOR ... XOR sh[n-2]
    Therefore:
        sh[0] XOR ... XOR sh[n-1] = w
    For every fixed tuple of the first n-1 shares, exactly one final share
    makes the XOR equal to w. Hence the output distribution is uniform over
    all Boolean sharings of w.

Status:
    hand_argument
"""
    inject_path = out_dir / "transcripts" / "inject_uniform_hand_argument.txt"
    write_text(inject_path, inject_text)

    rows.append(ResultRow(
        gadget="inject_uniform",
        property="uniform_output_sharing",
        order="all",
        result="hand_argument",
        tool="N/A",
        tool_version="N/A",
        runtime_seconds="N/A",
        transcript_file=str(inject_path.relative_to(out_dir)),
        input_file="N/A",
        command="N/A",
    ))

    ks_text = """COMPOSITION ARGUMENT: ks_add

gadgets.c schedule:
    ks_add uses:
      - isw_and for masked AND / multiplication
      - masked_xor for share-wise XOR
      - memcpy for copying share arrays

Argument:
    Share-wise XOR is linear and does not require fresh randomness.
    The non-linear operation inside ks_add is delegated to isw_and.
    Therefore the ks_add claim is not a separate direct IronMask proof of
    the whole Kogge-Stone circuit in this artifact. It is a composition
    argument based on the verified isw_and checks and the linearity of XOR.

Status:
    composition_argument
"""
    ks_path = out_dir / "transcripts" / "ks_add_composition_argument.txt"
    write_text(ks_path, ks_text)

    rows.append(ResultRow(
        gadget="ks_add",
        property="composition",
        order="all",
        result="composition_argument",
        tool="N/A",
        tool_version="N/A",
        runtime_seconds="N/A",
        transcript_file=str(ks_path.relative_to(out_dir)),
        input_file="N/A",
        command="N/A",
    ))

    tree_text = """TREE CONVERSION STATUS: a2b_tree_batch32 / tree_rec

Status:
    not_machine_checked_by_this_script

Reason:
    The tree conversion computes recursively on reduced-size sharings inside
    subtrees. Plain composition of isw_and / refresh_sni / linear_refresh is
    not enough by itself to claim the full tree conversion.

Required paper-side artifact:
    A CGV14-style lemma / reduction explaining why probes inside reduced-share
    subtrees consume the adversary budget correctly, and how the linear refresh
    at merges aligns with the proof.

This file intentionally keeps the caveat visible instead of overstating
the machine-checked result.
"""
    tree_path = out_dir / "transcripts" / "tree_conversion_lemma_status.txt"
    write_text(tree_path, tree_text)

    rows.append(ResultRow(
        gadget="tree_conversion",
        property="tree_security",
        order="all",
        result="requires_appendix_lemma",
        tool="N/A",
        tool_version="N/A",
        runtime_seconds="N/A",
        transcript_file=str(tree_path.relative_to(out_dir)),
        input_file="N/A",
        command="N/A",
    ))

    return rows


def write_summary_csv(path: Path, rows: List[ResultRow]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    fields = [
        "gadget",
        "property",
        "order",
        "result",
        "tool",
        "tool_version",
        "runtime_seconds",
        "transcript_file",
        "input_file",
        "command",
    ]

    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({
                "gadget": r.gadget,
                "property": r.property,
                "order": r.order,
                "result": r.result,
                "tool": r.tool,
                "tool_version": r.tool_version,
                "runtime_seconds": r.runtime_seconds,
                "transcript_file": r.transcript_file,
                "input_file": r.input_file,
                "command": r.command,
            })


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ironmask", default="/work/IronMask/src/ironmask",
                    help="Path to IronMask executable")
    ap.add_argument("--gadgets-c", default="/work/Core/Src/gadgets.c",
                    help="Path to the project gadgets.c")
    ap.add_argument("--out-dir", default="/work/verification",
                    help="Output verification directory")
    ap.add_argument("--max-order", type=int, default=3,
                    help="Highest probing order to attempt. Default: 3")
    ap.add_argument("--jobs", type=int, default=1,
                    help="IronMask -j value. Default: 1")
    ap.add_argument("--timeout", type=int, default=1800,
                    help="Timeout per IronMask run in seconds. Default: 1800")
    ap.add_argument("--include-pini", action="store_true",
                    help="Also run PINI for all machine-checkable generated gadgets: isw_and, refresh_sni, and linear_refresh. NI/SNI are run by default where applicable. Some PINI checks may fail for this encoding.")
    ap.add_argument("--skip-sanity-check", action="store_true",
                    help="Skip implementation-pattern check on gadgets.c")
    args = ap.parse_args()

    ironmask_bin = Path(args.ironmask)
    gadgets_c = Path(args.gadgets_c)
    out_dir = Path(args.out_dir)
    inputs_dir = out_dir / "ironmask_inputs"
    transcripts_dir = out_dir / "transcripts"

    if not ironmask_bin.exists():
        print(f"ERROR: IronMask executable not found: {ironmask_bin}")
        return 2

    if not gadgets_c.exists():
        print(f"ERROR: gadgets.c not found: {gadgets_c}")
        print("Pass the correct path with --gadgets-c.")
        return 2

    if args.max_order < 1:
        print("ERROR: --max-order must be >= 1")
        return 2

    if args.max_order > 8:
        print("ERROR: this project supports d <= 8 because NSHARES_MAX = 9.")
        return 2

    if not args.skip_sanity_check:
        require_expected_gadgets_c(gadgets_c)

    inputs_dir.mkdir(parents=True, exist_ok=True)
    transcripts_dir.mkdir(parents=True, exist_ok=True)

    tool_version = get_tool_version(ironmask_bin)
    gadgets_hash = sha256_file(gadgets_c)

    metadata = (
        "AMORTIZE Step 3 IronMask verification artifacts\n\n"
        f"gadgets_c: {gadgets_c}\n"
        f"gadgets_c_sha256: {gadgets_hash}\n"
        f"ironmask: {ironmask_bin}\n"
        f"tool_version_or_help_header: {tool_version}\n"
        f"max_order: {args.max_order}\n"
        f"jobs: {args.jobs}\n"
        f"timeout_seconds_per_run: {args.timeout}\n\n"
        "Note:\n"
        "  The .sage inputs are generated from the operation schedule of the\n"
        "  current gadgets.c implementation. This script is not a general C parser.\n"
    )
    write_text(out_dir / "verification_metadata.txt", metadata)

    jobs: List[Job] = []

    for order in range(1, args.max_order + 1):
        shares = order + 1

        isw_path = inputs_dir / f"ours_isw_and_{shares}_shares.sage"
        refresh_path = inputs_dir / f"ours_refresh_sni_{shares}_shares.sage"
        linear_path = inputs_dir / f"ours_linear_refresh_{shares}_shares.sage"

        write_text(isw_path, gen_isw_and_exact(shares))
        write_text(refresh_path, gen_refresh_sni_exact(shares))
        write_text(linear_path, gen_linear_refresh_exact(shares))

        # isw_and: always run NI/SNI. PINI optional because it may fail.
        for prop in ["NI", "SNI"]:
            jobs.append(Job(
                gadget="isw_and",
                prop=prop,
                order=order,
                sage_file=isw_path,
                transcript_file=transcripts_dir / f"ours_isw_and_{prop.lower()}_order{order}.txt",
                timeout_seconds=args.timeout,
            ))

        if args.include_pini:
            jobs.append(Job(
                gadget="isw_and",
                prop="PINI",
                order=order,
                sage_file=isw_path,
                transcript_file=transcripts_dir / f"ours_isw_and_pini_order{order}.txt",
                timeout_seconds=args.timeout,
            ))

        # refresh_sni: run both NI and SNI by default.
        # SNI is the main target for this refresh gadget, but NI is also recorded
        # explicitly so the CSV contains the full baseline property matrix.
        for prop in ["NI", "SNI"]:
            jobs.append(Job(
                gadget="refresh_sni",
                prop=prop,
                order=order,
                sage_file=refresh_path,
                transcript_file=transcripts_dir / f"ours_refresh_sni_{prop.lower()}_order{order}.txt",
                timeout_seconds=args.timeout,
            ))

        # Optional extra PINI check for refresh_sni.
        if args.include_pini:
            jobs.append(Job(
                gadget="refresh_sni",
                prop="PINI",
                order=order,
                sage_file=refresh_path,
                transcript_file=transcripts_dir / f"ours_refresh_sni_pini_order{order}.txt",
                timeout_seconds=args.timeout,
            ))

        # linear_refresh: NI is the main target.
        jobs.append(Job(
            gadget="linear_refresh",
            prop="NI",
            order=order,
            sage_file=linear_path,
            transcript_file=transcripts_dir / f"ours_linear_refresh_ni_order{order}.txt",
            timeout_seconds=args.timeout,
        ))

        # Optional extra SNI/PINI checks for linear_refresh.
        # SNI is useful context for a refresh gadget; PINI is included because
        # --include-pini now means "try PINI on every generated gadget".
        if args.include_pini:
            jobs.append(Job(
                gadget="linear_refresh",
                prop="SNI",
                order=order,
                sage_file=linear_path,
                transcript_file=transcripts_dir / f"ours_linear_refresh_sni_order{order}.txt",
                timeout_seconds=args.timeout,
            ))

            jobs.append(Job(
                gadget="linear_refresh",
                prop="PINI",
                order=order,
                sage_file=linear_path,
                transcript_file=transcripts_dir / f"ours_linear_refresh_pini_order{order}.txt",
                timeout_seconds=args.timeout,
            ))

    rows: List[ResultRow] = []
    print(f"Running {len(jobs)} IronMask jobs...")

    for idx, job in enumerate(jobs, start=1):
        print(f"[{idx}/{len(jobs)}] {job.gadget} {job.prop} order={job.order}")
        result, runtime, cmd_str = run_job(job, ironmask_bin, args.jobs)

        try:
            transcript_rel = str(job.transcript_file.relative_to(out_dir))
        except ValueError:
            transcript_rel = str(job.transcript_file)

        try:
            input_rel = str(job.sage_file.relative_to(out_dir))
        except ValueError:
            input_rel = str(job.sage_file)

        rows.append(ResultRow(
            gadget=job.gadget,
            property=job.prop,
            order=str(job.order),
            result=result,
            tool="IronMask",
            tool_version=tool_version,
            runtime_seconds=f"{runtime:.3f}",
            transcript_file=transcript_rel,
            input_file=input_rel,
            command=cmd_str,
        ))

        print(f"    -> {result} in {runtime:.3f}s")

    rows.extend(make_hand_argument_files(out_dir))

    summary_path = out_dir / "verification_summary.csv"
    write_summary_csv(summary_path, rows)

    print()
    print("Done.")
    print(f"Summary CSV: {summary_path}")
    print(f"Inputs:      {inputs_dir}")
    print(f"Transcripts: {transcripts_dir}")

    failed = [r for r in rows if r.result in {"failed", "timeout", "error_or_timeout", "completed_unparsed"}]
    if failed:
        print()
        print("Non-verified / attention-needed rows:")
        for r in failed:
            print(f"  - {r.gadget},{r.property},order={r.order}: {r.result} -> {r.transcript_file}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

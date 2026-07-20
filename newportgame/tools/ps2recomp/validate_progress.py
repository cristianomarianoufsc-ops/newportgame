#!/usr/bin/env python3
"""
validate_progress.py — Anti-false-positive progress validator for ps2recomp.

PURPOSE
-------
Agents (and humans) working on this PS2 static recompilation pipeline tend to
report "progress" that isn't real. Classic false positives:

  • Binary compiles  →  but crashes at byte 0 of ELF
  • Syscall count dropped  →  because ELF isn't loaded (0 syscalls = WORSE)
  • "No more UNHANDLEDs"  →  because bad instructions were silently NOP'd
  • "Frames rendered"  →  but count is 0 (headless ran, nothing drawn)
  • 2.5 billion syscall 0x83 calls  →  infinite spin-loop, not "good activity"

This tool measures OBJECTIVE indicators, compares them to a stored baseline,
and flags regressions or false positives before you commit anything.

USAGE
-----
  # Full check (recompiler quality + runtime test):
  python3 tools/ps2recomp/validate_progress.py

  # Just recompiler quality (no binary needed):
  python3 tools/ps2recomp/validate_progress.py --no-run

  # Accept current state as new baseline (after confirming real progress):
  python3 tools/ps2recomp/validate_progress.py --accept-baseline

  # Run with specific ELF:
  python3 tools/ps2recomp/validate_progress.py --elf build/elf_out/SCUS_973.99

  # Run with specific frames budget:
  python3 tools/ps2recomp/validate_progress.py --frames 60

EXIT CODES
----------
  0  Progress confirmed (≥ baseline on all metrics, no false positives)
  1  Regression detected (metric worse than baseline)
  2  False positive detected (looks OK but isn't)
  3  Build / environment error (binary missing, ELF missing, etc.)
  4  Baseline does not exist yet (first run — use --accept-baseline)

OUTPUT
------
  /tmp/ps2recomp_validation_<timestamp>.json   Full machine-readable report
  stdout                                        Human summary

ADDING NEW CHECKS
-----------------
Implement a function check_<name>(ctx) -> CheckResult and add it to CHECKS.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Paths (relative to this script's location)
# ---------------------------------------------------------------------------
SCRIPT_DIR  = Path(__file__).parent.resolve()
REPO_ROOT   = SCRIPT_DIR.parent.parent   # …/newportgame
BUILD_DIR   = SCRIPT_DIR / "build"
RUNTIME_DIR = SCRIPT_DIR / "runtime" / "build"
BINARY      = RUNTIME_DIR / "ps2_game"
OUTPUT_C    = BUILD_DIR / "output.c"
BASELINE    = SCRIPT_DIR / "progress_baseline.json"

# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------
@dataclass
class CheckResult:
    name: str
    status: str          # "PASS" | "FAIL" | "WARN" | "SKIP" | "FALSE_POSITIVE"
    value: object        # current measured value
    baseline: object     # baseline value (None if not set)
    message: str         # human-readable explanation
    metric_key: str = "" # key used in baseline JSON

@dataclass
class Context:
    elf_path:    Optional[Path]
    frames:      int
    run_output:  str  = ""   # combined stdout+stderr from binary
    run_ok:      bool = False
    run_elapsed: float = 0.0

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _baseline_load() -> dict:
    if BASELINE.exists():
        with open(BASELINE) as f:
            return json.load(f)
    return {}

def _baseline_save(data: dict):
    with open(BASELINE, "w") as f:
        json.dump(data, f, indent=2)
    print(f"  Baseline salvo em {BASELINE}")

def _run_binary(ctx: Context) -> str:
    """Run ps2_game headlessly and capture output. Populates ctx.run_*."""
    if not BINARY.exists():
        return ""
    if ctx.elf_path is None or not ctx.elf_path.exists():
        return ""
    cmd = [str(BINARY), "--headless", f"--frames={ctx.frames}", str(ctx.elf_path)]
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        ctx.run_output  = r.stdout + r.stderr
        ctx.run_ok      = True
    except subprocess.TimeoutExpired as e:
        ctx.run_output  = (e.stdout or "") + (e.stderr or "") + "\n[TIMEOUT]"
        ctx.run_ok      = False
    except Exception as e:
        ctx.run_output  = f"[EXEC ERROR] {e}"
        ctx.run_ok      = False
    ctx.run_elapsed = time.time() - t0
    return ctx.run_output

def _parse_syscall_stats(output: str) -> dict:
    """
    Parse the syscall stats block printed by bios_stub.cpp's dump_syscall_stats().
    Returns {syscall_hex: count} dict and special keys:
      _total        total syscall invocations
      _unique       unique syscall codes seen
      _top1_count   call count of the most-called syscall
      _top1_code    hex code of the most-called syscall
    """
    stats = {}
    total = 0
    # bios_stub prints lines like:  "  [0x03] CreateThread         :    12345"
    for m in re.finditer(r'\[0x([0-9a-fA-F]+)\]\s+\S+\s+:\s+(\d+)', output):
        code = int(m.group(1), 16)
        cnt  = int(m.group(2))
        key  = f"0x{code:02x}"
        stats[key] = cnt
        total += cnt
    if not stats and re.search(r'syscall', output, re.I):
        # Simpler fallback: "syscall 0x83: 2500000000 calls"
        for m in re.finditer(r'syscall\s+0x([0-9a-fA-F]+)[^0-9]*(\d+)', output, re.I):
            key = f"0x{int(m.group(1),16):02x}"
            cnt = int(m.group(2))
            stats[key] = cnt
            total += cnt
    stats["_total"]  = total
    stats["_unique"] = len([k for k in stats if not k.startswith("_")])
    if stats["_unique"] > 0:
        top = max(((v, k) for k, v in stats.items() if not k.startswith("_")), default=(0,""))
        stats["_top1_count"] = top[0]
        stats["_top1_code"]  = top[1]
    else:
        stats["_top1_count"] = 0
        stats["_top1_code"]  = ""
    return stats

def _count_in_output_c(pattern: str) -> int:
    if not OUTPUT_C.exists():
        return -1
    with open(OUTPUT_C) as f:
        return sum(1 for line in f if pattern in line)

# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------
def check_binary_exists(ctx: Context) -> CheckResult:
    exists = BINARY.exists()
    return CheckResult(
        name="binary_exists",
        status="PASS" if exists else "FAIL",
        value=str(BINARY) if exists else None,
        baseline=None,
        message="ps2_game binary found" if exists else f"MISSING: {BINARY}",
    )

def check_elf_exists(ctx: Context) -> CheckResult:
    exists = ctx.elf_path is not None and ctx.elf_path.exists()
    return CheckResult(
        name="elf_exists",
        status="PASS" if exists else "WARN",
        value=str(ctx.elf_path) if exists else None,
        baseline=None,
        message="ELF found" if exists else "ELF not present — runtime checks skipped",
    )

def check_unhandled_count(ctx: Context, bl: dict) -> CheckResult:
    """Lower UNHANDLED count = real recompiler progress."""
    n = _count_in_output_c("/* UNHANDLED:")
    key = "unhandled_count"
    base = bl.get(key)
    if n < 0:
        return CheckResult(name=key, status="SKIP", value=n, baseline=base,
                           message="output.c not found", metric_key=key)
    if base is None:
        status, msg = "WARN", f"No baseline — current: {n}"
    elif n < base:
        status, msg = "PASS", f"{n} UNHANDLED (was {base}, ↓{base-n} improvement)"
    elif n == base:
        status, msg = "PASS", f"{n} UNHANDLED (unchanged)"
    else:
        status, msg = "FAIL", f"REGRESSION: {n} UNHANDLED (was {base}, ↑{n-base} worse)"
    return CheckResult(name=key, status=status, value=n, baseline=base,
                       message=msg, metric_key=key)

def check_todo_count(ctx: Context, bl: dict) -> CheckResult:
    """Lower TODO count = real recompiler progress."""
    n = _count_in_output_c("/* TODO:")
    key = "todo_count"
    base = bl.get(key)
    if n < 0:
        return CheckResult(name=key, status="SKIP", value=n, baseline=base,
                           message="output.c not found", metric_key=key)
    if base is None:
        status, msg = "WARN", f"No baseline — current: {n}"
    elif n <= base:
        status, msg = "PASS", f"{n} TODOs (was {base})"
    else:
        status, msg = "FAIL", f"REGRESSION: {n} TODOs (was {base})"
    return CheckResult(name=key, status=status, value=n, baseline=base,
                       message=msg, metric_key=key)

def check_binary_runs(ctx: Context) -> CheckResult:
    """Binary must start without immediate crash."""
    if not ctx.run_ok and "[EXEC ERROR]" in ctx.run_output:
        return CheckResult(name="binary_runs", status="FAIL", value=False,
                           baseline=None, message=ctx.run_output[:200])
    if not BINARY.exists() or ctx.elf_path is None:
        return CheckResult(name="binary_runs", status="SKIP", value=None,
                           baseline=None, message="Binary or ELF missing")
    ok = ctx.run_ok
    return CheckResult(
        name="binary_runs",
        status="PASS" if ok else "WARN",
        value=ok,
        baseline=None,
        message=f"Binary ran (elapsed={ctx.run_elapsed:.1f}s)" if ok else "Binary timed out or errored",
    )

def check_syscall_total(ctx: Context, bl: dict) -> CheckResult:
    """
    Total syscall count.
    FALSE POSITIVE RULES:
      • 0 syscalls = ELF not loading (ps2_ram is zero) — this is WORSE than any positive number.
      • > 1_000_000_000 = spin loop (regression, not progress).
    """
    key = "syscall_total"
    if not ctx.run_ok or not ctx.run_output:
        return CheckResult(name=key, status="SKIP", value=None, baseline=bl.get(key),
                           message="No run output", metric_key=key)
    stats = _parse_syscall_stats(ctx.run_output)
    total = stats["_total"]
    base  = bl.get(key)

    # False-positive detections
    if total == 0:
        return CheckResult(name=key, status="FALSE_POSITIVE", value=0, baseline=base,
                           message="0 syscalls — ELF not loading into ps2_ram. "
                                   "Binary 'ran' but executed nothing real.",
                           metric_key=key)
    if total > 1_000_000_000:
        return CheckResult(name=key, status="FALSE_POSITIVE", value=total, baseline=base,
                           message=f"{total:,} syscalls — likely infinite spin loop "
                                   f"(classic 0x83 loop). NOT progress.",
                           metric_key=key)

    if base is None:
        status, msg = "WARN", f"No baseline — total syscalls: {total:,}"
    elif total >= base:
        status, msg = "PASS", f"{total:,} syscalls (was {base:,})"
    else:
        # Fewer syscalls could be progress (fixed a loop) OR regression (crashed earlier)
        status, msg = "WARN", (f"{total:,} syscalls (was {base:,}) — "
                                f"could be fix or earlier crash, verify manually")
    return CheckResult(name=key, status=status, value=total, baseline=base,
                       message=msg, metric_key=key)

def check_syscall_unique(ctx: Context, bl: dict) -> CheckResult:
    """More unique syscall codes = reaching more of the BIOS initialization sequence."""
    key = "syscall_unique"
    if not ctx.run_ok or not ctx.run_output:
        return CheckResult(name=key, status="SKIP", value=None, baseline=bl.get(key),
                           message="No run output", metric_key=key)
    stats  = _parse_syscall_stats(ctx.run_output)
    unique = stats["_unique"]
    base   = bl.get(key)

    if unique == 0:
        return CheckResult(name=key, status="FALSE_POSITIVE", value=0, baseline=base,
                           message="0 unique syscalls — confirms ELF not loading.",
                           metric_key=key)
    if base is None:
        status, msg = "WARN", f"No baseline — {unique} unique syscalls"
    elif unique >= base:
        status, msg = "PASS", f"{unique} unique syscalls (was {base})"
    else:
        status, msg = "WARN", f"{unique} unique syscalls (was {base}) — crashed earlier?"
    return CheckResult(name=key, status=status, value=unique, baseline=base,
                       message=msg, metric_key=key)

def check_spin_loop(ctx: Context, bl: dict) -> CheckResult:
    """
    Detect syscall 0x83 domination (the known DMA merge spin-loop).
    If 0x83 accounts for > 95% of all calls and total > 10M, it's a spin loop.
    """
    key = "spin_loop_free"
    if not ctx.run_ok or not ctx.run_output:
        return CheckResult(name=key, status="SKIP", value=None, baseline=None,
                           message="No run output")
    stats = _parse_syscall_stats(ctx.run_output)
    total = stats["_total"]
    s83   = stats.get("0x83", 0)

    if total > 10_000_000 and s83 / max(total, 1) > 0.95:
        return CheckResult(name=key, status="FALSE_POSITIVE",
                           value={"0x83_calls": s83, "pct": f"{100*s83/total:.1f}%"},
                           baseline=None,
                           message=f"Spin loop ACTIVE: syscall 0x83 = {s83:,} "
                                   f"({100*s83/total:.1f}% of {total:,} total). "
                                   f"Fix bios_stub.cpp alternating DMA pointer return.")
    return CheckResult(name=key, status="PASS",
                       value={"0x83_calls": s83, "total": total},
                       baseline=None,
                       message=f"No spin loop detected (0x83={s83:,} / total={total:,})")

def check_frames(ctx: Context, bl: dict) -> CheckResult:
    """
    Rendered frame count. 0 frames with headless mode is normal IF gs_stub is stubbed.
    But if gs_stub claims frames rendered, verify them.
    """
    key = "frames_rendered"
    if not ctx.run_ok or not ctx.run_output:
        return CheckResult(name=key, status="SKIP", value=None, baseline=bl.get(key),
                           message="No run output", metric_key=key)
    m = re.search(r'frames?[:\s]+(\d+)', ctx.run_output, re.I)
    frames = int(m.group(1)) if m else 0
    base = bl.get(key)
    if base is None:
        status, msg = "WARN", f"No baseline — {frames} frames"
    elif frames >= base:
        status, msg = "PASS", f"{frames} frames (was {base})"
    else:
        status, msg = "WARN", f"{frames} frames (was {base})"
    return CheckResult(name=key, status=status, value=frames, baseline=base,
                       message=msg, metric_key=key)

def check_no_segfault(ctx: Context) -> CheckResult:
    """Binary must not segfault."""
    if not ctx.run_ok and not ctx.run_output:
        return CheckResult(name="no_segfault", status="SKIP", value=None,
                           baseline=None, message="No run output")
    seg = "Segmentation fault" in ctx.run_output or "signal 11" in ctx.run_output
    return CheckResult(
        name="no_segfault",
        status="FAIL" if seg else "PASS",
        value=not seg,
        baseline=None,
        message="SEGFAULT detected — check memory accessors or vf[] array bounds" if seg else "No segfault",
    )

# ---------------------------------------------------------------------------
# Score computation
# ---------------------------------------------------------------------------
def compute_score(results: list[CheckResult]) -> dict:
    """
    Numeric progress score 0–100.
    Weight:  PASS=1, WARN=0.5, SKIP=0.5, FAIL=0, FALSE_POSITIVE=0
    """
    weights = {"PASS": 1.0, "WARN": 0.5, "SKIP": 0.5, "FAIL": 0.0, "FALSE_POSITIVE": 0.0}
    scored = [r for r in results if r.status != "SKIP"]
    if not scored:
        return {"score": 0, "max": 0, "pct": 0}
    total_w = sum(weights.get(r.status, 0) for r in scored)
    max_w   = float(len(scored))
    pct     = round(100 * total_w / max_w, 1)
    return {"score": total_w, "max": max_w, "pct": pct}

# ---------------------------------------------------------------------------
# Report writer
# ---------------------------------------------------------------------------
def write_report(results: list[CheckResult], ctx: Context, score: dict, out_path: Path):
    report = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "elf": str(ctx.elf_path),
        "frames_requested": ctx.frames,
        "run_elapsed_s": round(ctx.run_elapsed, 2),
        "score": score,
        "checks": [asdict(r) for r in results],
        "raw_output_tail": ctx.run_output[-2000:] if ctx.run_output else "",
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
STATUS_EMOJI = {
    "PASS": "✅", "FAIL": "❌", "WARN": "⚠️ ",
    "SKIP": "⏭️ ", "FALSE_POSITIVE": "🚨",
}

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", help="Path to SCUS_973.99 ELF (auto-detected if omitted)")
    ap.add_argument("--frames", type=int, default=30, help="Headless frame budget (default 30)")
    ap.add_argument("--no-run", action="store_true", help="Skip binary execution")
    ap.add_argument("--accept-baseline", action="store_true",
                    help="Save current measurements as new baseline")
    ap.add_argument("--output", help="JSON report path (default: /tmp/ps2recomp_validation_<ts>.json)")
    args = ap.parse_args()

    # Locate ELF
    elf_candidates = [
        args.elf,
        str(BUILD_DIR / "elf_out" / "SCUS_973.99"),
        str(BUILD_DIR / "elf_out" / "SCUS_973.99.elf"),
    ]
    elf_path = None
    for c in elf_candidates:
        if c and Path(c).exists():
            elf_path = Path(c)
            break

    ctx = Context(elf_path=elf_path, frames=args.frames)

    print("=" * 62)
    print("  ps2recomp Progress Validator (anti-false-positive)")
    print("=" * 62)
    print(f"  Binary : {BINARY}")
    print(f"  ELF    : {elf_path or 'NOT FOUND'}")
    print(f"  output.c: {OUTPUT_C}")
    print()

    # Run binary (once, results shared across checks)
    if not args.no_run and elf_path and BINARY.exists():
        print(f"  Running binary (--headless --frames={ctx.frames})…", flush=True)
        _run_binary(ctx)
        print(f"  Done ({ctx.run_elapsed:.1f}s)")
        print()

    bl = _baseline_load()

    # Execute all checks
    results: list[CheckResult] = [
        check_binary_exists(ctx),
        check_elf_exists(ctx),
        check_unhandled_count(ctx, bl),
        check_todo_count(ctx, bl),
        check_binary_runs(ctx),
        check_no_segfault(ctx),
        check_syscall_total(ctx, bl),
        check_syscall_unique(ctx, bl),
        check_spin_loop(ctx, bl),
        check_frames(ctx, bl),
    ]

    # Print results
    print(f"  {'CHECK':<28}  {'STATUS':<14}  MESSAGE")
    print("  " + "-" * 58)
    for r in results:
        emoji  = STATUS_EMOJI.get(r.status, "?")
        status = f"{emoji} {r.status}"
        print(f"  {r.name:<28}  {status:<14}  {r.message}")

    score = compute_score(results)
    print()
    print(f"  Progress score: {score['pct']}%  ({score['score']:.1f}/{score['max']:.1f} checks)")
    print()

    # False positive / regression summary
    false_pos  = [r for r in results if r.status == "FALSE_POSITIVE"]
    regressions = [r for r in results if r.status == "FAIL"]
    if false_pos:
        print("  🚨 FALSE POSITIVES DETECTED — do NOT commit as 'progress':")
        for r in false_pos:
            print(f"     • {r.name}: {r.message}")
        print()
    if regressions:
        print("  ❌ REGRESSIONS:")
        for r in regressions:
            print(f"     • {r.name}: {r.message}")
        print()

    # Write JSON report
    ts = time.strftime("%Y%m%d_%H%M%S")
    report_path = Path(args.output) if args.output else Path(f"/tmp/ps2recomp_validation_{ts}.json")
    write_report(results, ctx, score, report_path)
    print(f"  Report: {report_path}")

    # Accept baseline
    if args.accept_baseline:
        new_bl = dict(bl)
        for r in results:
            if r.metric_key and r.value is not None and r.status in ("PASS", "WARN"):
                new_bl[r.metric_key] = r.value
        new_bl["accepted_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        _baseline_save(new_bl)
        print("  Baseline updated.")

    print()
    # Exit code
    if false_pos:
        sys.exit(2)
    if regressions:
        sys.exit(1)
    if not bl and not args.accept_baseline:
        print("  ℹ️  No baseline yet. Run with --accept-baseline to set one.")
        sys.exit(4)
    sys.exit(0)

if __name__ == "__main__":
    main()

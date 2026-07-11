#!/usr/bin/env python3
"""Compare assembler opt report against a C++ opt_release baseline report."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(p: Path) -> dict:
    with p.open() as f:
        return json.load(f)


def find_cpp_baseline() -> Path | None:
    """Resolve C++ baseline JSON without hard-coding private monorepo names."""
    env = os.environ.get("CPP_BASELINE_JSON")
    if env:
        p = Path(env).expanduser()
        if p.exists():
            return p

    standalone = ROOT / "docs" / "baselines" / "cpp_opt_release.json"
    if standalone.exists():
        return standalone

    # Monorepo: any sibling directory's opt report except this repo / asm-ish names
    parent = ROOT.parent
    if parent.is_dir():
        skip = {ROOT.name.lower(), "asm_test", "hft-asm-l2-conflator"}
        for p in sorted(parent.glob("*/benchmark_report_opt_release.json")):
            if p.parent.name.lower() in skip:
                continue
            return p
    return None


def find_asm_report() -> Path | None:
    for p in (
        ROOT / "benchmark_report_opt_release.json",
        ROOT / "results" / "benchmark_report_opt_release.json",
    ):
        if p.exists():
            return p
    return None


def main() -> int:
    legacy_path = find_cpp_baseline()
    asm_path = find_asm_report()
    if legacy_path is None:
        print(
            "missing C++ baseline report; set CPP_BASELINE_JSON or place "
            "docs/baselines/cpp_opt_release.json",
            file=sys.stderr,
        )
        return 1
    if asm_path is None:
        print(
            "missing assembler opt report "
            "(benchmark_report_opt_release.json or results/…)",
            file=sys.stderr,
        )
        return 1

    L = load(legacy_path)
    A = load(asm_path)
    lm = L["metrics"]["latency_ns"]
    am = A["metrics"]["latency_ns"]
    lt = L["metrics"]["throughput_msg_per_sec"]
    at = A["metrics"]["throughput_msg_per_sec"]

    def row(name: str, legacy: float, asm: float, lower_better: bool = True) -> None:
        if legacy == 0:
            ratio = float("inf")
        else:
            ratio = asm / legacy
        if lower_better:
            better = "BETTER" if asm < legacy else "WORSE"
            speedup = legacy / asm if asm else float("inf")
            print(
                f"  {name:12}  cpp={legacy:12.2f}  asm={asm:12.2f}  "
                f"ratio={ratio:.3f}  ({better}, {speedup:.2f}x lower)"
            )
        else:
            better = "BETTER" if asm > legacy else "WORSE"
            speedup = asm / legacy if legacy else float("inf")
            print(
                f"  {name:12}  cpp={legacy:12.2f}  asm={asm:12.2f}  "
                f"ratio={ratio:.3f}  ({better}, {speedup:.2f}x higher)"
            )

    print(f"C++ baseline: {legacy_path}")
    print(
        f"  agent={L.get('agent_name')} build={L['environment'].get('build_type')}"
    )
    print(f"Assembler:    {asm_path}")
    print(
        f"  agent={A.get('agent_name')} build={A['environment'].get('build_type')}"
    )
    print()
    print("Throughput (msg/s):")
    row("throughput", lt, at, lower_better=False)
    print("Latency (ns):")
    for k in ("min", "mean", "p50", "p90", "p99", "p99_9", "max"):
        row(k, float(lm[k]), float(am[k]), lower_better=True)

    ok = am["mean"] < lm["mean"] * 0.7 and am["p99"] < lm["p99"] * 0.7
    print()
    print(
        "Gate (mean & p99 < 70% of C++ baseline):",
        "PASS" if ok else "FAIL",
    )
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())

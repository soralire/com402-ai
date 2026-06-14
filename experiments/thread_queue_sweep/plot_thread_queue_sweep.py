#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Plot the original thread/queue sweep experiment results.

Default input:
  results/thread_queue_sweep/thread_queue_clean.csv

Default output:
  results/thread_queue_sweep/figures/
"""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


MODE_ORDER = ["random", "csma", "aimd"]
MODE_LABELS = {
    "random": "Random",
    "csma": "CSMA-like",
    "aimd": "AIMD",
}

NUMERIC_COLS = [
    "load",
    "seed",
    "attempts",
    "success",
    "retry",
    "backoff",
    "delay_p50",
    "delay_p95",
    "delay_p99",
    "goodput",
    "threads",
    "seconds",
    "mem_node",
    "cpu_node",
    "mem_mb",
    "touches_per_req",
    "latency_samples",
    "queue_depth",
    "device_workers",
    "avg_cwnd",
    "avg_inflight",
    "max_inflight",
]


def parse_args() -> argparse.Namespace:
    project_dir = Path(__file__).resolve().parents[2]
    result_dir = project_dir / "results" / "thread_queue_sweep"
    parser = argparse.ArgumentParser(
        description="Generate figures for the thread/queue sweep experiment."
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=result_dir / "thread_queue_clean.csv",
        help="Input clean CSV from run_thread_queue_sweep.sh.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=result_dir / "figures",
        help="Output directory for figures.",
    )
    parser.add_argument(
        "--format",
        dest="fmt",
        choices=["png", "pdf", "svg"],
        default="png",
        help="Figure format.",
    )
    parser.add_argument("--dpi", type=int, default=300, help="DPI for PNG output.")
    return parser.parse_args()


def read_results(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(f"CSV file does not exist: {path}")

    df = pd.read_csv(path)
    df = df[df["track"].astype(str).str.lower() != "track"].copy()
    df["track"] = df["track"].astype(str).str.strip().str.lower()
    for col in NUMERIC_COLS:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    df = df.dropna(subset=["track", "load", "goodput"]).copy()
    df["load"] = df["load"].astype(int)
    for col in ["delay_p50", "delay_p95", "delay_p99"]:
        if col in df.columns:
            df[f"{col}_ms"] = df[col] / 1_000_000.0
    if {"attempts", "retry"}.issubset(df.columns):
        df["retry_rate_pct"] = df["retry"] / df["attempts"].where(df["attempts"] > 0) * 100.0
    if {"attempts", "backoff"}.issubset(df.columns):
        df["backoff_rate_pct"] = df["backoff"] / df["attempts"].where(df["attempts"] > 0) * 100.0
    return df.reset_index(drop=True)


def ordered_modes(values: pd.Series) -> list[str]:
    present = list(dict.fromkeys(values.astype(str)))
    known = [mode for mode in MODE_ORDER if mode in present]
    unknown = sorted(mode for mode in present if mode not in MODE_ORDER)
    return known + unknown


def savefig(out_dir: Path, stem: str, fmt: str, dpi: int) -> Path:
    out_path = out_dir / f"{stem}.{fmt}"
    plt.tight_layout()
    plt.savefig(out_path, dpi=dpi, bbox_inches="tight")
    plt.close()
    return out_path


def config_columns(df: pd.DataFrame) -> list[str]:
    cols: list[str] = []
    for col in ["threads", "queue_depth", "device_workers"]:
        if col in df.columns and df[col].nunique(dropna=True) > 1:
            cols.append(col)
    return cols


def config_label(row: pd.Series, cols: list[str]) -> str:
    if not cols:
        return "all configs"
    parts = []
    for col in cols:
        value = row[col]
        if pd.notna(value) and float(value).is_integer():
            value = int(value)
        parts.append(f"{col}={value}")
    return ", ".join(parts)


def plot_metric(df: pd.DataFrame, metric: str, ylabel: str, title: str, stem: str, out_dir: Path, fmt: str, dpi: int) -> Path | None:
    if metric not in df.columns:
        return None

    group_cols = ["track", "load", *config_columns(df)]
    agg = df.groupby(group_cols, dropna=False)[metric].mean().reset_index()
    if agg.empty:
        return None

    cfg_cols = config_columns(df)
    if cfg_cols:
        configs = agg[cfg_cols].drop_duplicates().sort_values(cfg_cols).reset_index(drop=True)
    else:
        configs = pd.DataFrame([{}])

    n_panels = len(configs)
    fig, axes = plt.subplots(n_panels, 1, figsize=(8, max(4.5, 3.5 * n_panels)), squeeze=False)
    for ax, (_, cfg) in zip(axes.ravel(), configs.iterrows()):
        sub_cfg = agg
        if cfg_cols:
            mask = pd.Series(True, index=agg.index)
            for col in cfg_cols:
                mask &= agg[col].eq(cfg[col])
            sub_cfg = agg[mask]

        for mode in ordered_modes(sub_cfg["track"]):
            sub = sub_cfg[sub_cfg["track"] == mode].sort_values("load")
            if sub.empty:
                continue
            ax.plot(sub["load"], sub[metric], marker="o", linewidth=2, label=MODE_LABELS.get(mode, mode))

        ax.set_title(config_label(cfg, cfg_cols))
        ax.set_xlabel("Load (%)")
        ax.set_ylabel(ylabel)
        ax.grid(True, linestyle="--", alpha=0.45)
        ax.legend(fontsize=9)

    fig.suptitle(title, y=1.01, fontsize=14)
    return savefig(out_dir, stem, fmt, dpi)


def main() -> None:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    df = read_results(args.csv)
    generated: list[Path] = []

    specs = [
        ("goodput", "Goodput (requests/s)", "Thread/queue sweep: goodput", "thread_queue_goodput"),
        ("delay_p99_ms", "p99 latency (ms)", "Thread/queue sweep: p99 latency", "thread_queue_p99_latency"),
        ("retry_rate_pct", "Retry rate (%)", "Thread/queue sweep: retry rate", "thread_queue_retry_rate"),
        ("backoff_rate_pct", "Backoff rate (%)", "Thread/queue sweep: backoff rate", "thread_queue_backoff_rate"),
        ("avg_cwnd", "Average cwnd", "Thread/queue sweep: AIMD cwnd", "thread_queue_avg_cwnd"),
        ("avg_inflight", "Average in-flight requests", "Thread/queue sweep: in-flight requests", "thread_queue_avg_inflight"),
    ]
    for metric, ylabel, title, stem in specs:
        path = plot_metric(df, metric, ylabel, title, stem, args.out_dir, args.fmt, args.dpi)
        if path is not None:
            generated.append(path)

    summary_path = args.out_dir / "summary_by_config.csv"
    summary_cols = [c for c in ["goodput", "delay_p50_ms", "delay_p95_ms", "delay_p99_ms", "retry_rate_pct", "backoff_rate_pct", "avg_cwnd", "avg_inflight"] if c in df.columns]
    df.groupby(["track", "load", *config_columns(df)], dropna=False)[summary_cols].mean().reset_index().to_csv(summary_path, index=False)
    generated.append(summary_path)

    manifest = args.out_dir / "figure_manifest.txt"
    with manifest.open("w", encoding="utf-8") as f:
        f.write("Thread/queue sweep figures\n")
        f.write(f"Input CSV: {args.csv}\n")
        f.write(f"Rows: {len(df)}\n\n")
        for path in generated:
            f.write(path.name + "\n")
    generated.append(manifest)

    print(f"Loaded {len(df)} rows from {args.csv}")
    print(f"Generated {len(generated)} files under {args.out_dir}")
    for path in generated:
        print(f"  - {path.name}")


if __name__ == "__main__":
    main()

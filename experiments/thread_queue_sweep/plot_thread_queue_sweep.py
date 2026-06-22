#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Plot the worker/credit oversubscription experiment.

The policies have different pacing semantics:

* random blocks until a fabric credit is available;
* csma and aimd retain a logical request across failed admission attempts;
* aimd uses one global shared congestion window across all workers.

Latency for retry-based policies includes admission waiting and backoff.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


MODE_ORDER = ["random", "csma", "aimd"]
MODE_LABELS = {
    "random": "Blocking",
    "csma": "Backoff retry",
    "aimd": "Global AIMD",
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
    "elapsed_s",
    "global_avg_cwnd",
    "global_avg_inflight",
    "global_max_inflight",
]


def parse_args() -> argparse.Namespace:
    project_dir = Path(__file__).resolve().parents[2]
    result_dir = project_dir / "results" / "thread_queue_sweep"
    parser = argparse.ArgumentParser(
        description="Plot policy behavior against worker/credit oversubscription."
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
        help="Output directory for figures and summaries.",
    )
    parser.add_argument("--load", type=int, default=100, help="Load value to plot.")
    parser.add_argument(
        "--queue-depth", type=int, default=4, help="Queue-credit depth to plot."
    )
    parser.add_argument(
        "--device-workers", type=int, default=1, help="Device-worker count to plot."
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


def safe_ratio(numerator: pd.Series, denominator: pd.Series) -> pd.Series:
    return numerator / denominator.where(denominator > 0)


def read_results(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(f"CSV file does not exist: {path}")

    df = pd.read_csv(path)
    if "track" not in df.columns:
        raise ValueError("CSV must contain a 'track' column")

    df = df[df["track"].astype(str).str.lower() != "track"].copy()
    df["track"] = df["track"].astype(str).str.strip().str.lower()
    for col in NUMERIC_COLS:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    required = {
        "track",
        "load",
        "seed",
        "attempts",
        "success",
        "retry",
        "delay_p99",
        "goodput",
        "threads",
        "queue_depth",
        "device_workers",
    }
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"CSV is missing required columns: {sorted(missing)}")

    df = df.dropna(subset=list(required)).copy()
    df["delay_p99_ms"] = df["delay_p99"] / 1_000_000.0
    df["acceptance_rate_pct"] = (
        safe_ratio(df["success"], df["attempts"]) * 100.0
    )
    df["admission_rejections_per_completion"] = safe_ratio(
        df["retry"], df["success"]
    )
    df["oversubscription"] = safe_ratio(df["threads"], df["queue_depth"])
    if "global_avg_cwnd" in df.columns:
        df["global_window_pressure"] = safe_ratio(
            df["global_avg_cwnd"], df["queue_depth"]
        )

    if "elapsed_s" in df.columns:
        df["measured_completion_rate"] = safe_ratio(df["success"], df["elapsed_s"])
    else:
        # Older CSV files remain readable. Their existing goodput was already
        # calculated from measured elapsed time inside the C program.
        df["measured_completion_rate"] = df["goodput"]

    return df.reset_index(drop=True)


def filter_experiment(df: pd.DataFrame, args: argparse.Namespace) -> pd.DataFrame:
    selected = df[
        (df["load"] == args.load)
        & (df["queue_depth"] == args.queue_depth)
        & (df["device_workers"] == args.device_workers)
    ].copy()
    if selected.empty:
        available = (
            df[["load", "queue_depth", "device_workers"]]
            .drop_duplicates()
            .sort_values(["load", "queue_depth", "device_workers"])
        )
        raise ValueError(
            "No rows match "
            f"load={args.load}, queue_depth={args.queue_depth}, "
            f"device_workers={args.device_workers}.\n"
            f"Available configurations:\n{available.to_string(index=False)}"
        )
    return selected


def ordered_modes(values: pd.Series) -> list[str]:
    present = list(dict.fromkeys(values.astype(str)))
    known = [mode for mode in MODE_ORDER if mode in present]
    unknown = sorted(mode for mode in present if mode not in MODE_ORDER)
    return known + unknown


def aggregate_metric(df: pd.DataFrame, metric: str) -> pd.DataFrame:
    grouped = (
        df.groupby(["track", "threads", "queue_depth", "oversubscription"])[metric]
        .agg(mean="mean", std="std", count="count")
        .reset_index()
    )
    # Student-t critical values are more appropriate than 1.96 for the small
    # seed counts used by these experiments (normally n=5).
    t95 = {
        1: 0.0,
        2: 12.706,
        3: 4.303,
        4: 3.182,
        5: 2.776,
        6: 2.571,
        7: 2.447,
        8: 2.365,
        9: 2.306,
        10: 2.262,
    }
    grouped["critical95"] = grouped["count"].map(
        lambda count: t95.get(int(count), 1.96)
    )
    grouped["ci95"] = (
        grouped["critical95"]
        * grouped["std"].fillna(0.0)
        / grouped["count"].clip(lower=1).map(math.sqrt)
    )
    return grouped


def savefig(out_dir: Path, stem: str, fmt: str, dpi: int) -> Path:
    out_path = out_dir / f"{stem}.{fmt}"
    plt.tight_layout()
    plt.savefig(out_path, dpi=dpi, bbox_inches="tight")
    plt.close()
    return out_path


def format_oversubscription_axis(ax: plt.Axes, values: pd.Series) -> None:
    ticks = sorted(values.dropna().unique())
    if ticks and min(ticks) > 0:
        ax.set_xscale("log", base=2)
        ax.set_xticks(ticks)
        ax.set_xticklabels([f"{value:g}" for value in ticks])
    ax.axvline(1.0, color="black", linestyle=":", linewidth=1.2, alpha=0.8)
    ax.grid(True, linestyle="--", alpha=0.4)


def plot_vs_oversubscription(
    df: pd.DataFrame,
    metric: str,
    ylabel: str,
    title: str,
    stem: str,
    out_dir: Path,
    fmt: str,
    dpi: int,
    ylim: tuple[float, float] | None = None,
) -> Path:
    agg = aggregate_metric(df, metric)
    plt.figure(figsize=(8.2, 5.2))
    ax = plt.gca()

    for mode in ordered_modes(agg["track"]):
        sub = agg[agg["track"] == mode].sort_values("oversubscription")
        ax.errorbar(
            sub["oversubscription"],
            sub["mean"],
            yerr=sub["ci95"],
            marker="o",
            linewidth=2,
            capsize=3,
            label=MODE_LABELS.get(mode, mode),
        )

    format_oversubscription_axis(ax, agg["oversubscription"])
    ax.set_xlabel("Worker/credit oversubscription (threads / queue depth)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if ylim is not None:
        ax.set_ylim(*ylim)
    ax.legend()
    return savefig(out_dir, stem, fmt, dpi)


def plot_tradeoff(
    df: pd.DataFrame, out_dir: Path, fmt: str, dpi: int
) -> Path:
    goodput = aggregate_metric(df, "measured_completion_rate")
    latency = aggregate_metric(df, "delay_p99_ms")
    agg = goodput.merge(
        latency,
        on=["track", "threads", "queue_depth", "oversubscription"],
        suffixes=("_goodput", "_p99"),
    )

    plt.figure(figsize=(8.2, 5.5))
    ax = plt.gca()
    for mode in ordered_modes(agg["track"]):
        sub = agg[agg["track"] == mode].sort_values("oversubscription")
        ax.errorbar(
            sub["mean_p99"],
            sub["mean_goodput"],
            xerr=sub["ci95_p99"],
            yerr=sub["ci95_goodput"],
            marker="o",
            linewidth=2,
            capsize=3,
            label=MODE_LABELS.get(mode, mode),
        )
        for _, row in sub.iterrows():
            ax.annotate(
                f"{row['oversubscription']:g}x",
                (row["mean_p99"], row["mean_goodput"]),
                xytext=(4, 4),
                textcoords="offset points",
                fontsize=8,
            )

    ax.set_xlabel("Logical-request end-to-end p99 latency (ms)")
    ax.set_ylabel("Completed goodput (requests/s)")
    ax.set_title("Goodput vs logical-request p99\n(labels show worker/credit ratio)")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend()
    return savefig(out_dir, "goodput_vs_logical_request_p99_tradeoff", fmt, dpi)


def plot_aimd_global_window(
    df: pd.DataFrame, out_dir: Path, fmt: str, dpi: int
) -> Path | None:
    required = {"global_avg_cwnd", "global_avg_inflight"}
    if not required.issubset(df.columns):
        return None

    aimd = df[df["track"] == "aimd"].copy()
    if aimd.empty or aimd["global_avg_cwnd"].fillna(0).max() <= 0:
        return None

    cwnd = aggregate_metric(aimd, "global_avg_cwnd")
    inflight = aggregate_metric(aimd, "global_avg_inflight")

    plt.figure(figsize=(8.2, 5.2))
    ax = plt.gca()
    for agg, label, marker in [
        (cwnd, "Global time-weighted cwnd", "o"),
        (inflight, "Global time-weighted in-flight", "s"),
    ]:
        sub = agg.sort_values("oversubscription")
        ax.errorbar(
            sub["oversubscription"],
            sub["mean"],
            yerr=sub["ci95"],
            marker=marker,
            linewidth=2,
            capsize=3,
            label=label,
        )

    format_oversubscription_axis(ax, cwnd["oversubscription"])
    ax.set_xlabel("Worker/credit ratio (threads / queue depth)")
    ax.set_ylabel("Requests")
    ax.set_title("Global AIMD window and in-flight requests")
    ax.legend()
    return savefig(out_dir, "aimd_global_window", fmt, dpi)


def write_summary(df: pd.DataFrame, out_dir: Path) -> Path:
    metrics = [
        "measured_completion_rate",
        "delay_p99_ms",
        "acceptance_rate_pct",
        "admission_rejections_per_completion",
        "avg_cwnd",
        "avg_inflight",
        "global_avg_cwnd",
        "global_avg_inflight",
        "global_max_inflight",
        "global_window_pressure",
    ]
    metrics = [metric for metric in metrics if metric in df.columns]
    summary = df.groupby(
        ["track", "threads", "queue_depth", "oversubscription"], dropna=False
    )[metrics].agg(["mean", "std", "count"])
    summary.columns = [
        "_".join(column).strip("_") for column in summary.columns.to_flat_index()
    ]
    summary = summary.reset_index()
    path = out_dir / "summary_by_oversubscription.csv"
    summary.to_csv(path, index=False)
    return path


def main() -> None:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    all_rows = read_results(args.csv)
    df = filter_experiment(all_rows, args)

    generated = [
        plot_vs_oversubscription(
            df,
            "measured_completion_rate",
            "Completed goodput (requests/s)",
            "Completed goodput vs worker/credit oversubscription",
            "oversubscription_vs_goodput",
            args.out_dir,
            args.fmt,
            args.dpi,
        ),
        plot_vs_oversubscription(
            df,
            "delay_p99_ms",
            "Logical-request end-to-end p99 latency (ms)",
            "Logical-request p99 vs worker/credit oversubscription",
            "oversubscription_vs_logical_request_p99",
            args.out_dir,
            args.fmt,
            args.dpi,
        ),
        plot_vs_oversubscription(
            df,
            "acceptance_rate_pct",
            "Admission-attempt success: success / attempts (%)",
            "Admission-attempt success vs worker/credit oversubscription",
            "oversubscription_vs_acceptance",
            args.out_dir,
            args.fmt,
            args.dpi,
            ylim=(0.0, 105.0),
        ),
        plot_vs_oversubscription(
            df,
            "admission_rejections_per_completion",
            "Admission rejections / completed request",
            "Admission rejections vs worker/credit oversubscription",
            "oversubscription_vs_rejections_per_completion",
            args.out_dir,
            args.fmt,
            args.dpi,
        ),
        plot_tradeoff(df, args.out_dir, args.fmt, args.dpi),
        write_summary(df, args.out_dir),
    ]
    aimd_window = plot_aimd_global_window(
        df, args.out_dir, args.fmt, args.dpi
    )
    if aimd_window is not None:
        generated.append(aimd_window)

    manifest = args.out_dir / "figure_manifest.txt"
    with manifest.open("w", encoding="utf-8") as file:
        file.write("Worker/credit oversubscription figures\n")
        file.write(f"Input CSV: {args.csv}\n")
        file.write(
            f"Filter: load={args.load}, queue_depth={args.queue_depth}, "
            f"device_workers={args.device_workers}\n"
        )
        file.write(f"Rows used: {len(df)}\n\n")
        file.write(
            "Interpretation: retry-based policy p99 includes admission waiting "
            "and backoff; acceptance is admission-attempt success rate.\n\n"
        )
        for path in generated:
            file.write(path.name + "\n")
    generated.append(manifest)

    print(f"Loaded {len(all_rows)} rows from {args.csv}")
    print(f"Used {len(df)} rows after the fixed-configuration filter")
    print(f"Generated {len(generated)} files under {args.out_dir}")
    for path in generated:
        print(f"  - {path.name}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Comprehensive plotting program for cxl_numa_csma thread/queue sweep results.

It reads either thread_queue_clean.csv or raw CSV files that contain repeated
headers, normalizes old/new schemas, derives key metrics, and generates figures
that show throughput, latency, contention, scaling, and throughput-latency
trade-offs.

Usage from project root:
    python3 experiments/thread_queue_sweep/plot_thread_queue_comprehensive.py

Common custom usage:
    python3 experiments/thread_queue_sweep/plot_thread_queue_comprehensive.py \
        --csv results/thread_queue_sweep/thread_queue_clean.csv \
        --out-dir results/thread_queue_sweep/figures_comprehensive \
        --format png --dpi 300
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from typing import Iterable, Sequence

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


MODE_ORDER = ["random", "csma", "aimd"]
MODE_LABELS = {
    "random": "Random (blocking)",
    "csma": "CSMA-like (backoff)",
    "aimd": "AIMD (adaptive)",
}

LATENCY_COLS = ["delay_p50", "delay_p95", "delay_p99"]
BASE_NUMERIC_COLS = [
    "load", "seed", "attempts", "success", "retry", "backoff",
    "delay_p50", "delay_p95", "delay_p99", "goodput", "threads", "seconds",
    "mem_node", "cpu_node", "mem_mb", "touches_per_req", "latency_samples",
    "queue_depth", "device_workers", "avg_cwnd", "avg_inflight", "max_inflight",
]
CONFIG_CANDIDATES = ["threads", "queue_depth", "device_workers"]


def parse_args() -> argparse.Namespace:
    project_dir = Path(__file__).resolve().parents[2]
    result_dir = project_dir / "results" / "thread_queue_sweep"
    return argparse.ArgumentParser(
        description="Generate comprehensive figures for cxl_numa_csma experiment CSV results."
    ).parse_args(namespace=argparse.Namespace(
        csv=result_dir / "thread_queue_clean.csv",
        out_dir=result_dir / "figures_comprehensive",
        fmt="png",
        dpi=300,
    ))


def parse_cli() -> argparse.Namespace:
    project_dir = Path(__file__).resolve().parents[2]
    result_dir = project_dir / "results" / "thread_queue_sweep"
    parser = argparse.ArgumentParser(
        description="Generate comprehensive figures for cxl_numa_csma experiment CSV results."
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=result_dir / "thread_queue_clean.csv",
        help="Input CSV. Can be clean CSV or raw CSV with repeated headers.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=result_dir / "figures_comprehensive",
        help="Directory for generated figures and summary CSV files.",
    )
    parser.add_argument(
        "--format",
        dest="fmt",
        choices=["png", "pdf", "svg"],
        default="png",
        help="Figure output format.",
    )
    parser.add_argument("--dpi", type=int, default=300, help="DPI for raster output.")
    return parser.parse_args()


def read_results(csv_path: Path) -> pd.DataFrame:
    if not csv_path.exists():
        raise FileNotFoundError(f"CSV file does not exist: {csv_path}")

    df = pd.read_csv(csv_path)

    # Raw files contain one header before every row. Remove those duplicated
    # header lines after pandas has parsed the first header.
    if "track" in df.columns:
        df = df[df["track"].astype(str).str.lower() != "track"].copy()

    # Normalize mode names.
    if "track" not in df.columns:
        raise ValueError("CSV must contain a 'track' column.")
    df["track"] = df["track"].astype(str).str.strip().str.lower()

    # Convert all known numeric columns that are present.
    for col in BASE_NUMERIC_COLS:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    # Keep only known modes unless the experiment added new policies. Unknown
    # modes are still kept and plotted after the known order.
    df = df.dropna(subset=["load", "goodput"])
    df["load"] = df["load"].astype(int)

    return df.reset_index(drop=True)


def safe_div(num: pd.Series, den: pd.Series) -> pd.Series:
    return np.where(den.to_numpy(dtype=float) > 0, num.to_numpy(dtype=float) / den.to_numpy(dtype=float), np.nan)


def add_derived_metrics(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()

    for col in LATENCY_COLS:
        if col in df.columns:
            df[f"{col}_ms"] = df[col] / 1_000_000.0

    if {"retry", "attempts"}.issubset(df.columns):
        df["retry_rate_pct"] = safe_div(df["retry"], df["attempts"]) * 100.0
    if {"backoff", "attempts"}.issubset(df.columns):
        df["backoff_rate_pct"] = safe_div(df["backoff"], df["attempts"]) * 100.0
    if {"success", "attempts"}.issubset(df.columns):
        df["success_rate_pct"] = safe_div(df["success"], df["attempts"]) * 100.0
    if {"goodput", "threads"}.issubset(df.columns):
        df["goodput_per_thread"] = safe_div(df["goodput"], df["threads"])
    if {"delay_p99", "delay_p50"}.issubset(df.columns):
        df["tail_amp_p99_p50"] = safe_div(df["delay_p99"], df["delay_p50"])
    if {"queue_depth", "threads"}.issubset(df.columns):
        df["credits_per_thread"] = safe_div(df["queue_depth"], df["threads"])

    return df


def present_varying_config_cols(df: pd.DataFrame) -> list[str]:
    cols: list[str] = []
    for col in CONFIG_CANDIDATES:
        if col in df.columns and df[col].notna().any() and df[col].nunique(dropna=True) > 1:
            cols.append(col)
    return cols


def config_label(row: pd.Series, config_cols: Sequence[str]) -> str:
    if not config_cols:
        return "all configs"
    parts = []
    for col in config_cols:
        value = row[col]
        if pd.isna(value):
            continue
        if float(value).is_integer():
            value = int(value)
        parts.append(f"{col}={value}")
    return ", ".join(parts) if parts else "all configs"


def ordered_tracks(values: Iterable[str]) -> list[str]:
    values = list(dict.fromkeys(str(v) for v in values))
    known = [m for m in MODE_ORDER if m in values]
    unknown = sorted(v for v in values if v not in MODE_ORDER)
    return known + unknown


def savefig(out_dir: Path, stem: str, fmt: str, dpi: int) -> Path:
    out_path = out_dir / f"{stem}.{fmt}"
    plt.tight_layout()
    plt.savefig(out_path, dpi=dpi, bbox_inches="tight")
    plt.close()
    return out_path


def aggregate_for_load(df: pd.DataFrame, metric: str, config_cols: Sequence[str]) -> pd.DataFrame:
    group_cols = ["track", "load", *config_cols]
    agg = (
        df.groupby(group_cols, dropna=False)[metric]
          .agg(mean="mean", std="std", count="count")
          .reset_index()
    )
    return agg


def make_config_table(agg: pd.DataFrame, config_cols: Sequence[str]) -> pd.DataFrame:
    if not config_cols:
        table = pd.DataFrame({"config_label": ["all configs"]})
        return table
    table = agg[list(config_cols)].drop_duplicates().sort_values(list(config_cols)).reset_index(drop=True)
    table["config_label"] = table.apply(lambda r: config_label(r, config_cols), axis=1)
    return table


def plot_metric_vs_load(
    df: pd.DataFrame,
    out_dir: Path,
    metric: str,
    ylabel: str,
    title: str,
    stem: str,
    fmt: str,
    dpi: int,
    config_cols: Sequence[str],
) -> Path | None:
    if metric not in df.columns:
        return None

    agg = aggregate_for_load(df.dropna(subset=[metric]), metric, config_cols)
    if agg.empty:
        return None

    config_table = make_config_table(agg, config_cols)
    n_panels = len(config_table)
    ncols = min(2, n_panels)
    nrows = math.ceil(n_panels / ncols)
    fig, axes = plt.subplots(nrows=nrows, ncols=ncols, figsize=(7.5 * ncols, 4.8 * nrows), squeeze=False)
    axes_flat = axes.ravel()

    tracks = ordered_tracks(agg["track"].unique())
    for ax_idx, (_, cfg_row) in enumerate(config_table.iterrows()):
        ax = axes_flat[ax_idx]
        if config_cols:
            mask = pd.Series(True, index=agg.index)
            for col in config_cols:
                mask &= agg[col].eq(cfg_row[col])
            sub_cfg = agg[mask]
            ax_title = cfg_row["config_label"]
        else:
            sub_cfg = agg
            ax_title = "All configurations"

        for track in tracks:
            sub = sub_cfg[sub_cfg["track"] == track].sort_values("load")
            if sub.empty:
                continue
            label = MODE_LABELS.get(track, track)
            ax.plot(sub["load"], sub["mean"], marker="o", linewidth=2, label=label)
            if sub["count"].max() > 1 and sub["std"].notna().any():
                lower = sub["mean"] - sub["std"].fillna(0)
                upper = sub["mean"] + sub["std"].fillna(0)
                ax.fill_between(sub["load"].to_numpy(), lower.to_numpy(), upper.to_numpy(), alpha=0.12)

        ax.set_title(ax_title)
        ax.set_xlabel("Offered load (%)")
        ax.set_ylabel(ylabel)
        ax.grid(True, linestyle="--", alpha=0.45)
        ax.legend(fontsize=9)

    for j in range(n_panels, len(axes_flat)):
        axes_flat[j].axis("off")

    fig.suptitle(title, y=1.02, fontsize=14)
    return savefig(out_dir, stem, fmt, dpi)


def plot_latency_percentiles_by_policy(
    df: pd.DataFrame,
    out_dir: Path,
    fmt: str,
    dpi: int,
    config_cols: Sequence[str],
) -> list[Path]:
    required = ["delay_p50_ms", "delay_p95_ms", "delay_p99_ms"]
    if not all(c in df.columns for c in required):
        return []

    paths: list[Path] = []
    # Average over config dimensions here, because this plot focuses on tail shape
    # rather than a specific hardware-like configuration.
    group_cols = ["track", "load"]
    agg = df.groupby(group_cols, dropna=False)[required].mean().reset_index()

    for track in ordered_tracks(agg["track"].unique()):
        sub = agg[agg["track"] == track].sort_values("load")
        if sub.empty:
            continue
        plt.figure(figsize=(8, 5))
        plt.plot(sub["load"], sub["delay_p50_ms"], marker="o", linewidth=2, label="p50")
        plt.plot(sub["load"], sub["delay_p95_ms"], marker="s", linewidth=2, label="p95")
        plt.plot(sub["load"], sub["delay_p99_ms"], marker="^", linewidth=2, label="p99")
        plt.xlabel("Offered load (%)")
        plt.ylabel("End-to-end request latency (ms)")
        plt.title(f"Latency percentiles vs load: {MODE_LABELS.get(track, track)}")
        plt.grid(True, linestyle="--", alpha=0.45)
        plt.legend()
        paths.append(savefig(out_dir, f"latency_percentiles_{track}", fmt, dpi))
    return paths


def plot_tradeoff(df: pd.DataFrame, out_dir: Path, fmt: str, dpi: int) -> Path | None:
    required = {"delay_p99_ms", "goodput", "track", "load"}
    if not required.issubset(df.columns):
        return None

    agg = (
        df.groupby(["track", "load"], dropna=False)[["delay_p99_ms", "goodput", "retry_rate_pct"]]
          .mean(numeric_only=True)
          .reset_index()
    )
    if agg.empty:
        return None

    plt.figure(figsize=(8, 5.5))
    for track in ordered_tracks(agg["track"].unique()):
        sub = agg[agg["track"] == track].sort_values("load")
        if sub.empty:
            continue
        plt.plot(sub["delay_p99_ms"], sub["goodput"], marker="o", linewidth=2, label=MODE_LABELS.get(track, track))
        for _, row in sub.iterrows():
            plt.annotate(str(int(row["load"])), (row["delay_p99_ms"], row["goodput"]), xytext=(4, 3), textcoords="offset points", fontsize=8)

    plt.xlabel("p99 latency (ms)")
    plt.ylabel("Goodput (requests/s)")
    plt.title("Goodput vs p99 latency trade-off\n(point labels are offered load percentages)")
    plt.grid(True, linestyle="--", alpha=0.45)
    plt.legend()
    return savefig(out_dir, "tradeoff_goodput_vs_p99", fmt, dpi)


def plot_config_bars(
    df: pd.DataFrame,
    out_dir: Path,
    metric: str,
    ylabel: str,
    title: str,
    stem: str,
    fmt: str,
    dpi: int,
    config_cols: Sequence[str],
) -> Path | None:
    if metric not in df.columns or not config_cols:
        return None

    work = df.dropna(subset=[metric]).copy()
    if work.empty:
        return None
    work["config_label"] = work.apply(lambda r: config_label(r, config_cols), axis=1)
    agg = work.groupby(["config_label", "track"], dropna=False)[metric].mean().reset_index()
    configs = list(agg["config_label"].drop_duplicates())
    tracks = ordered_tracks(agg["track"].unique())

    x = np.arange(len(configs))
    width = 0.8 / max(1, len(tracks))
    plt.figure(figsize=(max(9, 1.9 * len(configs)), 5.2))
    for idx, track in enumerate(tracks):
        sub = agg[agg["track"] == track].set_index("config_label").reindex(configs)
        offsets = x - 0.4 + width / 2 + idx * width
        plt.bar(offsets, sub[metric].to_numpy(), width=width, label=MODE_LABELS.get(track, track))

    plt.xticks(x, configs, rotation=25, ha="right")
    plt.xlabel("Experiment configuration")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, axis="y", linestyle="--", alpha=0.45)
    plt.legend()
    return savefig(out_dir, stem, fmt, dpi)


def plot_heatmaps_if_possible(df: pd.DataFrame, out_dir: Path, fmt: str, dpi: int) -> list[Path]:
    if not {"track", "load", "threads", "queue_depth", "delay_p99_ms", "goodput"}.issubset(df.columns):
        return []
    if df["threads"].nunique(dropna=True) < 1 or df["queue_depth"].nunique(dropna=True) < 2:
        return []

    paths: list[Path] = []
    for metric, label, stem_prefix in [
        ("delay_p99_ms", "p99 latency (ms)", "heatmap_p99"),
        ("goodput", "Goodput (requests/s)", "heatmap_goodput"),
    ]:
        for threads in sorted(df["threads"].dropna().unique()):
            sub_thread = df[df["threads"] == threads]
            tracks = ordered_tracks(sub_thread["track"].unique())
            ncols = len(tracks)
            fig, axes = plt.subplots(1, ncols, figsize=(5.2 * ncols, 4.5), squeeze=False)
            axes_flat = axes.ravel()
            for ax, track in zip(axes_flat, tracks):
                sub = sub_thread[sub_thread["track"] == track]
                pivot = sub.pivot_table(index="load", columns="queue_depth", values=metric, aggfunc="mean").sort_index()
                image = ax.imshow(pivot.to_numpy(), aspect="auto", origin="lower")
                ax.set_title(MODE_LABELS.get(track, track))
                ax.set_xlabel("queue_depth")
                ax.set_ylabel("Offered load (%)")
                ax.set_xticks(np.arange(len(pivot.columns)))
                ax.set_xticklabels([str(int(c)) for c in pivot.columns])
                ax.set_yticks(np.arange(len(pivot.index)))
                ax.set_yticklabels([str(int(i)) for i in pivot.index])
                for r in range(pivot.shape[0]):
                    for c in range(pivot.shape[1]):
                        val = pivot.iloc[r, c]
                        if pd.notna(val):
                            ax.text(c, r, f"{val:.1f}", ha="center", va="center", fontsize=8)
                fig.colorbar(image, ax=ax, fraction=0.046, pad=0.04, label=label)
            fig.suptitle(f"{label} heatmap at threads={int(threads)}", y=1.02, fontsize=14)
            paths.append(savefig(out_dir, f"{stem_prefix}_threads_{int(threads)}", fmt, dpi))
    return paths


def write_summaries(df: pd.DataFrame, out_dir: Path, config_cols: Sequence[str]) -> list[Path]:
    out_paths: list[Path] = []
    metrics = [
        c for c in [
            "goodput", "delay_p50_ms", "delay_p95_ms", "delay_p99_ms",
            "retry_rate_pct", "backoff_rate_pct", "success_rate_pct",
            "goodput_per_thread", "tail_amp_p99_p50", "avg_cwnd", "avg_inflight", "max_inflight",
        ] if c in df.columns
    ]
    group_cols = ["track", "load", *config_cols]
    summary = df.groupby(group_cols, dropna=False)[metrics].agg(["mean", "std", "count"])
    summary.columns = ["_".join(c).strip("_") for c in summary.columns.to_flat_index()]
    summary = summary.reset_index()
    path = out_dir / "summary_by_load_config.csv"
    summary.to_csv(path, index=False)
    out_paths.append(path)

    overall = df.groupby(["track"], dropna=False)[metrics].agg(["mean", "std", "min", "max"])
    overall.columns = ["_".join(c).strip("_") for c in overall.columns.to_flat_index()]
    overall = overall.reset_index()
    path = out_dir / "summary_overall_by_policy.csv"
    overall.to_csv(path, index=False)
    out_paths.append(path)
    return out_paths


def main() -> None:
    args = parse_cli()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    df = read_results(args.csv)
    df = add_derived_metrics(df)
    config_cols = present_varying_config_cols(df)

    generated: list[Path] = []
    generated.extend(write_summaries(df, args.out_dir, config_cols))

    metric_specs = [
        ("goodput", "Goodput (requests/s)", "Load vs goodput", "load_vs_goodput"),
        ("goodput_per_thread", "Goodput per worker thread (requests/s/thread)", "Load vs per-thread goodput", "load_vs_goodput_per_thread"),
        ("delay_p50_ms", "p50 latency (ms)", "Load vs p50 latency", "load_vs_p50_latency"),
        ("delay_p95_ms", "p95 latency (ms)", "Load vs p95 latency", "load_vs_p95_latency"),
        ("delay_p99_ms", "p99 latency (ms)", "Load vs p99 latency", "load_vs_p99_latency"),
        ("tail_amp_p99_p50", "p99 / p50 latency ratio", "Load vs tail-latency amplification", "load_vs_tail_amplification"),
        ("retry_rate_pct", "Retry rate (%)", "Load vs retry rate", "load_vs_retry_rate"),
        ("backoff_rate_pct", "Backoff rate (%)", "Load vs backoff rate", "load_vs_backoff_rate"),
        ("success_rate_pct", "Success / attempts (%)", "Load vs successful injection ratio", "load_vs_success_rate"),
        ("avg_cwnd", "Average cwnd", "Load vs average AIMD cwnd/window", "load_vs_avg_cwnd"),
        ("avg_inflight", "Average in-flight requests", "Load vs in-flight requests", "load_vs_avg_inflight"),
        ("max_inflight", "Maximum in-flight requests", "Load vs maximum in-flight requests", "load_vs_max_inflight"),
    ]

    for metric, ylabel, title, stem in metric_specs:
        path = plot_metric_vs_load(df, args.out_dir, metric, ylabel, title, stem, args.fmt, args.dpi, config_cols)
        if path is not None:
            generated.append(path)

    generated.extend(plot_latency_percentiles_by_policy(df, args.out_dir, args.fmt, args.dpi, config_cols))

    path = plot_tradeoff(df, args.out_dir, args.fmt, args.dpi)
    if path is not None:
        generated.append(path)

    for metric, ylabel, title, stem in [
        ("goodput", "Mean goodput (requests/s)", "Average goodput by configuration", "config_avg_goodput"),
        ("delay_p99_ms", "Mean p99 latency (ms)", "Average p99 latency by configuration", "config_avg_p99_latency"),
        ("retry_rate_pct", "Mean retry rate (%)", "Average retry rate by configuration", "config_avg_retry_rate"),
    ]:
        path = plot_config_bars(df, args.out_dir, metric, ylabel, title, stem, args.fmt, args.dpi, config_cols)
        if path is not None:
            generated.append(path)

    generated.extend(plot_heatmaps_if_possible(df, args.out_dir, args.fmt, args.dpi))

    manifest = args.out_dir / "figure_manifest.txt"
    with manifest.open("w", encoding="utf-8") as f:
        f.write("Generated cxl_numa_csma result figures and summaries\n")
        f.write(f"Input CSV: {args.csv}\n")
        f.write(f"Rows: {len(df)}\n")
        f.write(f"Config dimensions: {', '.join(config_cols) if config_cols else 'none'}\n\n")
        for path in generated:
            f.write(str(path.name) + "\n")
    generated.append(manifest)

    print(f"Loaded {len(df)} rows from {args.csv}")
    print(f"Detected varying config dimensions: {', '.join(config_cols) if config_cols else 'none'}")
    print(f"Generated {len(generated)} files under {args.out_dir}")
    for path in generated:
        print(f"  - {path.name}")


if __name__ == "__main__":
    main()

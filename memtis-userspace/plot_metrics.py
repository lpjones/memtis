#!/usr/bin/env python3
"""
Plot MEMTIS runtime statistics from benchmark result logs.

The script is intentionally text-log based: it reads dmesg.txt and pgmig.txt
from a run directory and writes PNGs back to that same directory by default.
"""

import argparse
import os
import re
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


DMESG_TS_RE = re.compile(r"^\[\s*([0-9]+(?:\.[0-9]+)?)\]\s*(.*)$")
SAMPLE_RE = re.compile(
    r"sample_period:\s*(\d+)\s*\|\|\s*"
    r"cputime:\s*(\d+)\s*\|\|\s*"
    r"hit_ratio:\s*(\d+)\s*\|\|\s*"
    r"promoted:\s*(\d+)\s*\|\|\s*"
    r"demoted:\s*(\d+)"
    # optional fast/slow access counts (newer kernels)
    r"(?:\s*\|\|\s*fast:\s*(\d+)\s*\|\|\s*slow:\s*(\d+))?"
)
PAGR_DBG_RE = re.compile(r"PAGR_DBG\[([^\]]+)\]\s+(\w+)\s+(.*)$")
PAGR_BATCH_RE = re.compile(r"PAGR_BATCH\s+(.*)$")
PAGR_LRU_RE = re.compile(r"PAGR_LRU_(PROMOTE|DEMOTE)\s+(.*)$")
KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)")


def parse_numeric_fields(text):
    return {key: int(value) for key, value in KV_RE.findall(text)}


def parse_dmesg_file(path):
    samples = []
    debug = defaultdict(list)
    batches = []
    lru_events = []

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            ts_match = DMESG_TS_RE.match(line.strip())
            if not ts_match:
                continue

            timestamp = float(ts_match.group(1))
            message = ts_match.group(2)

            sample_match = SAMPLE_RE.search(message)
            if sample_match:
                sample = {
                    "timestamp": timestamp,
                    "sample_period": int(sample_match.group(1)),
                    "cputime": int(sample_match.group(2)),
                    "hit_ratio": int(sample_match.group(3)),
                    "promoted": int(sample_match.group(4)),
                    "demoted": int(sample_match.group(5)),
                }
                if sample_match.group(6) is not None:
                    sample["fast"] = int(sample_match.group(6))
                    sample["slow"] = int(sample_match.group(7))
                samples.append(sample)
                continue

            debug_match = PAGR_DBG_RE.search(message)
            if debug_match:
                where, section, fields = debug_match.groups()
                record = parse_numeric_fields(fields)
                record["timestamp"] = timestamp
                record["where"] = where
                debug[section].append(record)
                continue

            batch_match = PAGR_BATCH_RE.search(message)
            if batch_match:
                record = parse_numeric_fields(batch_match.group(1))
                record["timestamp"] = timestamp
                batches.append(record)
                continue

            lru_match = PAGR_LRU_RE.search(message)
            if lru_match:
                action, fields = lru_match.groups()
                record = parse_numeric_fields(fields)
                record["timestamp"] = timestamp
                record["action"] = action.lower()
                lru_events.append(record)

    return {
        "samples": samples,
        "debug": debug,
        "batches": batches,
        "lru_events": lru_events,
    }


def parse_pgmig_file(path):
    metrics = defaultdict(list)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                value = int(parts[1])
            except ValueError:
                continue
            metrics[parts[0]].append(value)

    return metrics


def ensure_output_dir(path):
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)


def save_figure(fig, output_path):
    ensure_output_dir(output_path)
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return output_path


def relative_x(records, base=None):
    if not records:
        return [], 0
    if base is None:
        base = records[0]["timestamp"]
    return [record["timestamp"] - base for record in records], base


def plot_sample_stats(samples, title, output_path):
    if not samples:
        return None

    x, _ = relative_x(samples)
    hit_ratio = [sample["hit_ratio"] / 100.0 for sample in samples]

    fig, axes = plt.subplots(4, 1, figsize=(12, 12), sharex=True)
    fig.suptitle(title, fontsize=14)

    axes[0].plot(x, [sample["sample_period"] for sample in samples], linewidth=1.4)
    axes[0].set_ylabel("sample period")

    axes[1].plot(x, [sample["cputime"] for sample in samples], linewidth=1.4)
    axes[1].set_ylabel("CPU permille")

    axes[2].plot(x, hit_ratio, linewidth=1.4)
    axes[2].set_ylabel("hit ratio (%)")

    axes[3].plot(x, [sample["promoted"] for sample in samples], label="promoted", linewidth=1.4)
    axes[3].plot(x, [sample["demoted"] for sample in samples], label="demoted", linewidth=1.4)
    axes[3].set_ylabel("base pages/s")
    axes[3].legend(loc="best")

    for ax in axes:
        ax.grid(True, alpha=0.3)

    axes[-1].set_xlabel("time since first sample (s)")
    fig.tight_layout()
    return save_figure(fig, output_path)


def plot_single_metric(samples, series_list, title, ylabel, output_path):
    """PACT-style standalone plot: one axis, one line per (field, label)."""
    if not samples:
        return None

    x, _ = relative_x(samples)
    fig, ax = plt.subplots(figsize=(10, 5))

    plotted = False
    for field, label, transform in series_list:
        if not any(field in sample for sample in samples):
            continue
        y = [transform(sample) for sample in samples]
        ax.plot(x, y, label=label, linewidth=1.4)
        plotted = True

    if not plotted:
        plt.close(fig)
        return None

    ax.set_title(title)
    ax.set_xlabel("time since first sample (s)")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    fig.tight_layout()
    return save_figure(fig, output_path)


def percent_fast(sample):
    fast = sample.get("fast", 0)
    slow = sample.get("slow", 0)
    if fast + slow == 0:
        return 0.0
    return 100.0 * fast / (fast + slow)


def plot_pact_style(samples, title, output_dir):
    """Emit the standalone plots PACT's single_plots.sh produces."""
    outputs = []

    output = plot_single_metric(
        samples,
        [
            ("fast", "Fast accesses", lambda s: s.get("fast", 0)),
            ("slow", "Slow accesses", lambda s: s.get("slow", 0)),
        ],
        title + ": Accesses",
        "Accesses / s",
        os.path.join(output_dir, "accesses.png"),
    )
    if output:
        outputs.append(output)

    output = plot_single_metric(
        samples,
        [("fast", "Percent Fast mem", percent_fast)],
        title + ": Percent Fast Mem",
        "Percent (%)",
        os.path.join(output_dir, "percent.png"),
    )
    if output:
        outputs.append(output)

    output = plot_single_metric(
        samples,
        [
            ("promoted", "Promotions", lambda s: s.get("promoted", 0)),
            ("demoted", "Demotions", lambda s: s.get("demoted", 0)),
        ],
        title + ": Migrations",
        "Base pages / s",
        os.path.join(output_dir, "migrations.png"),
    )
    if output:
        outputs.append(output)

    output = plot_single_metric(
        samples,
        [("sample_period", "Sample period", lambda s: s.get("sample_period", 0))],
        title + ": PEBS Sample Period",
        "Sample period",
        os.path.join(output_dir, "sample_period.png"),
    )
    if output:
        outputs.append(output)

    return outputs


def debug_series(debug, section, field):
    return [
        (record["timestamp"], record[field])
        for record in debug.get(section, [])
        if field in record
    ]


def plot_series(ax, series, base, label):
    if not series:
        return False

    x = [timestamp - base for timestamp, _ in series]
    y = [value for _, value in series]
    ax.plot(x, y, label=label, linewidth=1.3)
    return True


def plot_pagr_debug(debug, title, output_path):
    if not any(debug.values()):
        return None

    timestamps = [
        record["timestamp"]
        for records in debug.values()
        for record in records
    ]
    if not timestamps:
        return None
    base = min(timestamps)

    groups = [
        (
            "Prediction counters",
            [
                ("pred", "selected", "selected"),
                ("pred", "cap_hit", "cap hit"),
                ("pred", "zero", "zero"),
                ("pred", "skip_time", "skip time"),
                ("pred", "skip_not_promotable", "skip not promotable"),
            ],
        ),
        (
            "Queue and migration entries",
            [
                ("queue", "accepted", "queue accepted"),
                ("queue", "process_mig_entries", "migrated entries"),
                ("queue", "process_mig_failed", "failed entries"),
                ("migrate", "success_entries", "migration success"),
                ("migrate", "failed", "migration failed"),
            ],
        ),
        (
            "Base-page movement",
            [
                ("queue", "process_mig_base", "PAGR migrated base"),
                ("migrate", "success_base", "migration success base"),
                ("migrate", "lru_promote_base", "LRU promote base"),
                ("migrate", "lru_demote_base", "LRU demote base"),
            ],
        ),
        (
            "State",
            [
                ("state", "entries", "entries"),
                ("state", "predicted_entries", "predicted entries"),
                ("state", "history", "history"),
                ("state", "queue", "queue"),
            ],
        ),
        (
            "Threshold and migration timing",
            [
                ("state", "threshold", "threshold"),
                ("state", "avg_dist", "avg dist"),
                ("state", "mig_queue", "mig queue"),
                ("state", "mig_move", "mig move"),
            ],
        ),
    ]

    fig, axes = plt.subplots(len(groups), 1, figsize=(13, 15), sharex=True)
    fig.suptitle(title, fontsize=14)

    for ax, (ylabel, fields) in zip(axes, groups):
        plotted = False
        for section, field, label in fields:
            plotted |= plot_series(ax, debug_series(debug, section, field), base, label)
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        if plotted:
            ax.legend(loc="best", fontsize="small")
        else:
            ax.text(0.5, 0.5, "no data", transform=ax.transAxes, ha="center", va="center")

    axes[-1].set_xlabel("time since first PAGR_DBG line (s)")
    fig.tight_layout()
    return save_figure(fig, output_path)


def plot_pagr_events(batches, lru_events, title, output_path):
    if not batches and not lru_events:
        return None

    timestamps = [record["timestamp"] for record in batches + lru_events]
    base = min(timestamps)

    fig, axes = plt.subplots(3, 1, figsize=(13, 10), sharex=True)
    fig.suptitle(title, fontsize=14)

    if batches:
        x, _ = relative_x(batches, base)
        axes[0].scatter(x, [record.get("migrated_base", 0) for record in batches],
                        label="migrated base", s=10, alpha=0.7)
        axes[0].scatter(x, [record.get("failed_entries", 0) for record in batches],
                        label="failed entries", s=10, alpha=0.7)
        axes[0].scatter(x, [record.get("pending", 0) for record in batches],
                        label="pending", s=10, alpha=0.7)
        axes[0].legend(loc="best", fontsize="small")
    else:
        axes[0].text(0.5, 0.5, "no PAGR_BATCH data", transform=axes[0].transAxes,
                     ha="center", va="center")
    axes[0].set_ylabel("PAGR batch")

    for action in ("promote", "demote"):
        records = [record for record in lru_events if record["action"] == action]
        if not records:
            continue
        x, _ = relative_x(records, base)
        axes[1].scatter(x, [record.get("reclaimed", record.get("promoted", 0)) for record in records],
                        label=action, s=10, alpha=0.7)
        axes[2].scatter(x, [record.get("requested", 0) for record in records],
                        label=action, s=10, alpha=0.7)

    axes[1].set_ylabel("LRU moved")
    axes[2].set_ylabel("LRU requested")

    for ax in axes:
        ax.grid(True, alpha=0.3)
        if ax.get_legend_handles_labels()[0]:
            ax.legend(loc="best", fontsize="small")

    axes[-1].set_xlabel("time since first PAGR event (s)")
    fig.tight_layout()
    return save_figure(fig, output_path)


def plot_pgmig_stats(metrics, title, output_path):
    if not metrics:
        return None

    fig, axes = plt.subplots(2, 1, figsize=(12, 8), sharex=False)
    fig.suptitle(title, fontsize=14)

    for name, values in sorted(metrics.items()):
        x = list(range(len(values)))
        axes[0].plot(x, values, label=name, linewidth=1.4)

        if len(values) > 1:
            delta_x = list(range(1, len(values)))
            deltas = [values[i] - values[i - 1] for i in range(1, len(values))]
            axes[1].plot(delta_x, deltas, label=name, linewidth=1.4)

    axes[0].set_ylabel("cumulative count")
    axes[1].set_ylabel("delta per sample")
    axes[1].set_xlabel("sample index (~1s)")

    for ax in axes:
        ax.grid(True, alpha=0.3)
        if ax.get_legend_handles_labels()[0]:
            ax.legend(loc="best")

    fig.tight_layout()
    return save_figure(fig, output_path)


def resolve_output(path, output_dir):
    if os.path.isabs(path):
        return path
    return os.path.join(output_dir, path)


def main():
    parser = argparse.ArgumentParser(
        description="Plot MEMTIS dmesg.txt and pgmig.txt runtime statistics"
    )
    parser.add_argument("dmesg_file", nargs="?", help="Path to dmesg.txt")
    parser.add_argument("--pgmig", help="Path to pgmig.txt")
    parser.add_argument("--output-dir", help="Directory for generated plots")
    parser.add_argument(
        "--output",
        help="Output path for the sample-period dmesg plot "
             "(kept for compatibility with older callers)",
    )
    parser.add_argument("--title", default="MEMTIS Runtime Statistics")
    args = parser.parse_args()

    if not args.dmesg_file and not args.pgmig:
        parser.error("provide a dmesg file and/or --pgmig")

    first_input = args.dmesg_file or args.pgmig
    output_dir = args.output_dir or os.path.dirname(os.path.abspath(first_input))
    outputs = []

    if args.dmesg_file:
        parsed = parse_dmesg_file(args.dmesg_file)
        sample_output = (
            resolve_output(args.output, output_dir)
            if args.output
            else os.path.join(output_dir, "dmesg_metrics.png")
        )

        output = plot_sample_stats(
            parsed["samples"],
            args.title + ": dmesg samples",
            sample_output,
        )
        if output:
            outputs.append(output)

        outputs.extend(plot_pact_style(parsed["samples"], args.title, output_dir))

        output = plot_pagr_debug(
            parsed["debug"],
            args.title + ": PAGR debug counters",
            os.path.join(output_dir, "dmesg_pagr_debug.png"),
        )
        if output:
            outputs.append(output)

        output = plot_pagr_events(
            parsed["batches"],
            parsed["lru_events"],
            args.title + ": PAGR migration events",
            os.path.join(output_dir, "dmesg_pagr_events.png"),
        )
        if output:
            outputs.append(output)

    if args.pgmig:
        output = plot_pgmig_stats(
            parse_pgmig_file(args.pgmig),
            args.title + ": pgmigrate_success",
            os.path.join(output_dir, "pgmig_stats.png"),
        )
        if output:
            outputs.append(output)

    if not outputs:
        print("No plottable data found", file=sys.stderr)
        return 1

    for output in outputs:
        print("Saved plot to {}".format(output))
    return 0


if __name__ == "__main__":
    sys.exit(main())

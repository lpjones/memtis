#!/usr/bin/env python3
"""
Plot script for MEMTIS dmesg logs.

Parses dmesg.txt files and plots metrics over time:
- sample_period
- cputime
- hit_ratio
- promoted (per second)
- demoted (per second)
"""

import os
import sys
import re
import matplotlib.pyplot as plt
import argparse

def parse_dmesg_line(line):
    """
    Parse a single dmesg line to extract timestamp and metrics.
    """
    # Extract timestamp from [  123456.789012] format
    timestamp_match = re.match(r"^\[\s*(\d+\.\d+)\s*\]", line.strip())
    if not timestamp_match:
        return None

    timestamp = float(timestamp_match.group(1))

    # Extract metrics
    pattern = r"sample_period:\s*(\d+)\s*\|\|\s*cputime:\s*(\d+)\s*\|\|\s*hit_ratio:\s*(\d+)\s*\|\|\s*promoted:\s*(\d+)\s*\|\|\s*demoted:\s*(\d+)"
    metrics_match = re.search(pattern, line)
    if not metrics_match:
        return None

    return {
        "timestamp": timestamp,
        "sample_period": int(metrics_match.group(1)),
        "cputime": int(metrics_match.group(2)),
        "hit_ratio": int(metrics_match.group(3)),
        "promoted": int(metrics_match.group(4)),
        "demoted": int(metrics_match.group(5))
    }

def parse_dmesg_file(filepath):
    """Parse the entire dmesg.txt file."""
    data = []
    with open(filepath, "r") as f:
        for line in f:
            parsed = parse_dmesg_line(line)
            if parsed:
                data.append(parsed)
    return data

def plot_metrics(data, title="MEMTIS Metrics Over Time", output_path=None):
    """Plot the metrics over time and save the figure to a file."""
    if not data:
        print("No data to plot")
        return

    timestamps = [d["timestamp"] for d in data]
    start_time = timestamps[0]
    relative_times = [t - start_time for t in timestamps]

    metrics = ["sample_period", "cputime", "hit_ratio", "promoted", "demoted"]
    titles = ["Sample Period", "CPU Usage (permille)", "DRAM Hit Ratio (%)", "Promotions/sec", "Demotions/sec"]
    colors = ["b", "r", "g", "m", "c"]

    fig, axes = plt.subplots(5, 1, figsize=(12, 15), sharex=True)
    fig.suptitle(title, fontsize=16)

    for i, (metric, title_text, color) in enumerate(zip(metrics, titles, colors)):
        values = [d[metric] for d in data]
        axes[i].plot(relative_times, values, f"{color}-", linewidth=1)
        axes[i].set_ylabel(title_text)
        axes[i].set_title(f"{title_text} Over Time")
        axes[i].grid(True, alpha=0.3)

    axes[-1].set_xlabel("Time (seconds)")
    plt.tight_layout()

    if output_path:
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        fig.savefig(output_path)
        print(f"Saved plot to {output_path}")

    plt.show()

def main():
    parser = argparse.ArgumentParser(description="Plot MEMTIS metrics from dmesg log")
    parser.add_argument("dmesg_file", help="Path to dmesg.txt file")
    parser.add_argument("--title", default="MEMTIS Metrics Over Time", help="Plot title")
    parser.add_argument("--output", help="Output filename (saved under results/ by default)")

    args = parser.parse_args()

    data = parse_dmesg_file(args.dmesg_file)
    if not data:
        print(f"No valid data found in {args.dmesg_file}")
        sys.exit(1)

    print(f"Parsed {len(data)} data points")

    dmesg_dir = os.path.dirname(os.path.abspath(args.dmesg_file))
    if args.output:
        output_path = args.output if os.path.isabs(args.output) else os.path.join(dmesg_dir, args.output)
    else:
        base_name = os.path.splitext(os.path.basename(args.dmesg_file))[0]
        output_path = os.path.join(dmesg_dir, f"{base_name}_metrics.png")

    plot_metrics(data, args.title, output_path)

if __name__ == "__main__":
    main()

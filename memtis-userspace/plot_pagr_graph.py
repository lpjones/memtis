#!/usr/bin/env python3
"""
Visualize one PAGR neighbor snapshot from memtis_pagr_graph.bin.

The graph log records accepted neighbor edges. A single source sample can have
several neighbor records, so this plot chooses one source sample and draws:
  - the sampled page as one dot
  - every recorded neighbor for that sample as another color

The y-axis is virtual address and the x-axis is sample time in cycles.
"""

import argparse
import csv
import os
import random
import struct
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


PAGR_GRAPH_MAGIC = 0x3146524752474150
PAGR_GRAPH_VERSION = 1

HEADER_STRUCT = struct.Struct("<QII")
RECORD_STRUCT = struct.Struct("<13Q4IB7x")

RECORD_FIELDS = (
    "log_cyc",
    "src_va",
    "dst_va",
    "src_pfn",
    "dst_pfn",
    "src_cyc",
    "dst_cyc",
    "src_ip",
    "dst_ip",
    "distance",
    "time_diff",
    "threshold",
    "avg_dist",
    "src_idx",
    "dst_idx",
    "slot",
    "replaced_idx",
    "event",
)

EVENT_NAMES = {
    1: "insert",
    2: "refresh",
    3: "replace",
}


def parse_int(value):
    return int(value, 0)


def choose_address_unit(values):
    base = min(values)
    span = max(values) - base

    if span >= 1024 ** 3:
        return base, 1024 ** 3, "GB"
    if span >= 1024 ** 2:
        return base, 1024 ** 2, "MB"
    return base, 1024, "KB"


def scale_address(value, base, unit):
    return (value - base) / unit


def edge_width(distance, min_distance, max_distance, min_width, max_width):
    if max_distance <= min_distance:
        return max_width

    closeness = (max_distance - distance) / (max_distance - min_distance)
    return min_width + closeness * (max_width - min_width)


def read_header(handle, path):
    data = handle.read(HEADER_STRUCT.size)
    if len(data) != HEADER_STRUCT.size:
        raise ValueError("{} is too small to contain a PAGR graph header".format(path))

    magic, version, record_size = HEADER_STRUCT.unpack(data)
    if magic != PAGR_GRAPH_MAGIC:
        raise ValueError(
            "{} has bad magic 0x{:x}; expected 0x{:x}".format(
                path, magic, PAGR_GRAPH_MAGIC
            )
        )
    if version != PAGR_GRAPH_VERSION:
        raise ValueError(
            "{} has unsupported graph version {}; expected {}".format(
                path, version, PAGR_GRAPH_VERSION
            )
        )
    if record_size != RECORD_STRUCT.size:
        raise ValueError(
            "{} uses record size {}; parser expects {}".format(
                path, record_size, RECORD_STRUCT.size
            )
        )

    return record_size


def iter_records(path, tail_records=None, limit_records=None):
    file_size = os.path.getsize(path)

    with open(path, "rb") as handle:
        record_size = read_header(handle, path)
        total_records = max(0, (file_size - HEADER_STRUCT.size) // record_size)

        start_record = 0
        records_to_read = total_records
        if tail_records is not None:
            start_record = max(0, total_records - tail_records)
            records_to_read = total_records - start_record
        if limit_records is not None:
            records_to_read = min(records_to_read, limit_records)

        handle.seek(HEADER_STRUCT.size + start_record * record_size)
        for offset in range(records_to_read):
            data = handle.read(record_size)
            if len(data) != record_size:
                break
            values = RECORD_STRUCT.unpack(data)
            record = dict(zip(RECORD_FIELDS, values))
            record["seq"] = start_record + offset
            yield record, total_records


def sample_key(record):
    return (
        record["src_va"],
        record["src_pfn"],
        record["src_cyc"],
        record["src_ip"],
    )


def load_snapshot_groups(path, tail_records, limit_records, min_neighbors):
    groups = defaultdict(list)
    total_records = 0
    read_records = 0

    for record, total_records in iter_records(path, tail_records, limit_records):
        read_records += 1
        if record["src_va"] == 0 or record["dst_va"] == 0:
            continue
        groups[sample_key(record)].append(record)

    if min_neighbors > 1:
        groups = {
            key: records
            for key, records in groups.items()
            if len(records) >= min_neighbors
        }

    return groups, total_records, read_records


def choose_snapshot(groups, args):
    if not groups:
        raise ValueError("no source samples with enough neighbors were found")

    candidates = list(groups.keys())

    if args.center_va is not None:
        candidates = [key for key in candidates if key[0] == args.center_va]
    if args.center_pfn is not None:
        candidates = [key for key in candidates if key[1] == args.center_pfn]
    if args.sample_seq is not None:
        candidates = [
            key for key in candidates
            if any(record["seq"] == args.sample_seq for record in groups[key])
        ]

    if (args.center_va is None and args.center_pfn is None and
            args.sample_seq is None and args.prefer_neighbors > 1):
        richer = [
            key for key in candidates
            if len(groups[key]) >= args.prefer_neighbors
        ]
        if richer:
            candidates = richer

    if not candidates:
        raise ValueError("no source sample matched the requested selector")

    if args.seed is None:
        rng = random.SystemRandom()
        return rng.choice(candidates)

    rng = random.Random(args.seed)
    return rng.choice(sorted(candidates))


def select_neighbors(records, max_neighbors):
    records = sorted(records, key=lambda record: (record["dst_cyc"], record["dst_va"]))
    if max_neighbors is None or len(records) <= max_neighbors:
        return records

    return sorted(
        records,
        key=lambda record: (record["distance"], -record["time_diff"]),
    )[:max_neighbors]


def write_summary(path, key, records, absolute_time):
    if not path:
        return

    src_va, src_pfn, src_cyc, src_ip = key
    base_cyc = 0 if absolute_time else src_cyc

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "role",
                "seq",
                "va",
                "pfn",
                "cycle",
                "x_cycle",
                "ip",
                "distance",
                "time_diff",
                "threshold",
                "avg_dist",
                "src_idx",
                "dst_idx",
                "slot",
                "event",
                "replaced_idx",
            ]
        )
        writer.writerow(
            [
                "sample",
                "",
                hex(src_va),
                hex(src_pfn),
                src_cyc,
                src_cyc - base_cyc,
                hex(src_ip),
                "",
                "",
                "",
                "",
                records[0]["src_idx"] if records else "",
                "",
                "",
                "",
                "",
            ]
        )
        for record in records:
            writer.writerow(
                [
                    "neighbor",
                    record["seq"],
                    hex(record["dst_va"]),
                    hex(record["dst_pfn"]),
                    record["dst_cyc"],
                    record["dst_cyc"] - base_cyc,
                    hex(record["dst_ip"]),
                    record["distance"],
                    record["time_diff"],
                    record["threshold"],
                    record["avg_dist"],
                    record["src_idx"],
                    record["dst_idx"],
                    record["slot"],
                    EVENT_NAMES.get(record["event"], record["event"]),
                    record["replaced_idx"],
                ]
            )


def plot_snapshot(key, records, args, stats):
    src_va, src_pfn, src_cyc, src_ip = key
    base_cyc = 0 if args.absolute_time else src_cyc

    sample_x = src_cyc - base_cyc
    va_values = [src_va] + [record["dst_va"] for record in records]
    va_base, va_unit, va_unit_name = choose_address_unit(va_values)
    sample_y = scale_address(src_va, va_base, va_unit)
    neighbor_x = [record["dst_cyc"] - base_cyc for record in records]
    neighbor_y = [
        scale_address(record["dst_va"], va_base, va_unit)
        for record in records
    ]
    distances = [record["distance"] for record in records]
    min_distance = min(distances) if distances else 0
    max_distance = max(distances) if distances else 0

    fig, ax = plt.subplots(figsize=(13, 8))
    ax.set_title(args.title)

    if records and not args.no_lines:
        for record, x, y in zip(records, neighbor_x, neighbor_y):
            ax.plot(
                [sample_x, x],
                [sample_y, y],
                color="0.72",
                linewidth=edge_width(record["distance"],
                                     min_distance,
                                     max_distance,
                                     args.min_edge_width,
                                     args.max_edge_width),
                alpha=0.68,
                zorder=1,
            )

    if records:
        ax.scatter(
            neighbor_x,
            neighbor_y,
            color="#1f77b4",
            s=args.neighbor_size,
            alpha=0.9,
            edgecolors="black",
            linewidths=0.35,
            label="neighbors",
            zorder=3,
        )

    ax.scatter(
        [sample_x],
        [sample_y],
        s=args.sample_size,
        color="#d62728",
        edgecolors="black",
        linewidths=0.8,
        marker="o",
        label="sample",
        zorder=4,
    )

    if args.label_neighbors:
        for record, x, y in zip(records, neighbor_x, neighbor_y):
            ax.text(
                x,
                y,
                "slot {}\n0x{:x}".format(record["slot"], record["dst_va"]),
                fontsize=7,
                ha="left",
                va="bottom",
            )

    if args.label_sample:
        ax.text(
            sample_x,
            sample_y,
            "sample\nva=0x{:x}\npfn=0x{:x}".format(src_va, src_pfn),
            fontsize=8,
            ha="right",
            va="top",
            bbox={"facecolor": "white", "edgecolor": "0.8", "alpha": 0.85},
        )

    ax.set_xlabel("cycle" if args.absolute_time else "cycles since sampled page")
    ax.set_ylabel(
        "virtual address offset from 0x{:x} ({})".format(
            va_base, va_unit_name
        )
    )
    ax.grid(True, alpha=0.28)
    ax.legend(loc="best")

    subtitle = (
        "sample va=0x{src_va:x} pfn=0x{src_pfn:x} ip=0x{src_ip:x}; "
        "{neighbors} neighbors; VA span={va_span:.2f} {va_unit}; "
        "read {read_records:,}/{total_records:,} records; "
        "{groups:,} source samples considered"
    ).format(
        src_va=src_va,
        src_pfn=src_pfn,
        src_ip=src_ip,
        neighbors=len(records),
        va_span=(max(va_values) - min(va_values)) / va_unit,
        va_unit=va_unit_name,
        read_records=stats["read_records"],
        total_records=stats["total_records"],
        groups=stats["groups"],
    )
    ax.text(
        0.01,
        0.01,
        subtitle,
        transform=ax.transAxes,
        fontsize=9,
        ha="left",
        va="bottom",
        bbox={"facecolor": "white", "edgecolor": "0.8", "alpha": 0.9},
    )

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    fig.savefig(args.output, dpi=180, bbox_inches="tight")
    plt.close(fig)


def default_output_path(graph_file):
    base, _ = os.path.splitext(graph_file)
    return base + ".png"


def main():
    parser = argparse.ArgumentParser(
        description="Plot one random PAGR source sample and its neighbors over time"
    )
    parser.add_argument("graph_file", help="Path to memtis_pagr_graph.bin")
    parser.add_argument("--output", help="Output PNG path")
    parser.add_argument("--summary", help="Optional CSV for the plotted snapshot")
    parser.add_argument("--title", default="PAGR Neighbor Snapshot")
    parser.add_argument(
        "--tail-records",
        type=int,
        default=200000,
        help="Read only the most recent N records by default",
    )
    parser.add_argument(
        "--all-records",
        action="store_true",
        help="Read the full graph file instead of only --tail-records",
    )
    parser.add_argument("--limit-records", type=int)
    parser.add_argument(
        "--min-neighbors",
        type=int,
        default=1,
        help="Only choose source samples with at least this many neighbors",
    )
    parser.add_argument(
        "--prefer-neighbors",
        type=int,
        default=2,
        help="Prefer random source samples with at least this many neighbors when possible",
    )
    parser.add_argument(
        "--max-neighbors",
        type=int,
        help="If set, draw only this many closest neighbors from the chosen sample",
    )
    parser.add_argument("--seed", type=int, help="Seed for reproducible random choice")
    parser.add_argument("--center-va", type=parse_int, help="Choose this source VA")
    parser.add_argument("--center-pfn", type=parse_int, help="Choose this source PFN")
    parser.add_argument("--sample-seq", type=int, help="Choose sample containing this record seq")
    parser.add_argument(
        "--absolute-time",
        action="store_true",
        help="Use absolute cycle values on the x-axis instead of cycles since source",
    )
    parser.add_argument("--no-lines", action="store_true")
    parser.add_argument(
        "--no-label-sample",
        dest="label_sample",
        action="store_false",
        help="Do not annotate the sampled page",
    )
    parser.add_argument("--label-neighbors", action="store_true")
    parser.add_argument("--sample-size", type=float, default=230.0)
    parser.add_argument("--neighbor-size", type=float, default=80.0)
    parser.add_argument("--min-edge-width", type=float, default=0.6)
    parser.add_argument("--max-edge-width", type=float, default=4.0)
    args = parser.parse_args()

    args.output = args.output or default_output_path(args.graph_file)
    if args.summary is None:
        base, _ = os.path.splitext(args.output)
        args.summary = base + "_edges.csv"

    tail_records = None if args.all_records else args.tail_records
    groups, total_records, read_records = load_snapshot_groups(
        args.graph_file,
        tail_records,
        args.limit_records,
        args.min_neighbors,
    )
    key = choose_snapshot(groups, args)
    records = select_neighbors(groups[key], args.max_neighbors)

    stats = {
        "total_records": total_records,
        "read_records": read_records,
        "groups": len(groups),
    }
    plot_snapshot(key, records, args, stats)
    write_summary(args.summary, key, records, args.absolute_time)

    print("Read {}/{} graph records".format(read_records, total_records))
    print(
        "Selected sample va=0x{:x} pfn=0x{:x} cyc={} with {} neighbors".format(
            key[0], key[1], key[2], len(records)
        )
    )
    print("Saved snapshot to {}".format(args.output))
    print("Saved snapshot summary to {}".format(args.summary))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        sys.exit(1)

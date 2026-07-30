#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/gapbs
GRAPH_DIR=/mnt/pact_storage/workloads/gapbs
GRAPH=${GRAPH_DIR}/kron28-degree12.sg

BENCH_RUN=("${BIN}/pr" -f "${GRAPH}" -i 20 -t 1e-4 -n 3)
BENCH_DEPS=("${BIN}/pr" "${GRAPH}")
BENCH_CATEGORY="graph"
BENCH_DESCRIPTION="GAP Benchmark Suite PageRank on a scale-28 Kronecker graph"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="382MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="740MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="1400MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="2520MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="4200MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="6300MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="70000MB"
fi

export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

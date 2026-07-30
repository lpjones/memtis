#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/XSBench/openmp-threading
BENCH_RUN=("${BIN}/XSBench" -t 8 -g 75000 -p 5000000)
BENCH_DEPS=("${BIN}/XSBench")
BENCH_CATEGORY="scientific"
BENCH_DESCRIPTION="XSBench event-based Monte Carlo proxy, 75,000 gridpoints"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

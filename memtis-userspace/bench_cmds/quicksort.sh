#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/quicksort

BENCH_RUN=("${BIN}/quicksort" 32768 16)
BENCH_DEPS=("${BIN}/quicksort")
BENCH_CATEGORY="sorting"
BENCH_DESCRIPTION="Parallel in-place quicksort of 32 GiB of random 64-bit keys"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

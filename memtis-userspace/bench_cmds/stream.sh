#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/stream
BENCH_RUN=("${BIN}/stream" 12288 50)
BENCH_DEPS=("${BIN}/stream")
BENCH_CATEGORY="bandwidth"
BENCH_DESCRIPTION="36 GiB STREAM memory-bandwidth workload"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

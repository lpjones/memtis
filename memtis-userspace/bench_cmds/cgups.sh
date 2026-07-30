#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/cgups
BENCH_RUN=("${BIN}/gups64-rw" 8 move 60 kill 120)
BENCH_DEPS=("${BIN}/gups64-rw")
BENCH_CATEGORY="prefetch"
BENCH_DESCRIPTION="32 GiB concurrent GUPS random-access and movement workload"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

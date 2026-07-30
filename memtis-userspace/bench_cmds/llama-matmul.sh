#!/bin/bash

BIN=/mnt/pact_storage/workloads/llama

BENCH_RUN=("${BIN}/benchmark-matmult" -t 8 -i 10)
BENCH_CWD=${BIN}
BENCH_DEPS=("${BIN}/benchmark-matmult")
BENCH_CATEGORY="prefetch"
BENCH_DESCRIPTION="llama.cpp explicit-prefetch matrix multiplication"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

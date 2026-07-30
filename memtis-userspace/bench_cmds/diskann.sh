#!/bin/bash

BIN_WORK_DIR=/mnt/pact_storage/workloads/DiskANN
BIN=${BIN_WORK_DIR}/target/release/diskann-benchmark
INPUT=${BIN_WORK_DIR}/diskann-benchmark/example/load.json
OUTPUT=${BIN_WORK_DIR}/dynamic-output.json
BENCH_RUN=("${BIN}" run --input-file "${INPUT}" --output-file "${OUTPUT}")
BENCH_CWD=${BIN_WORK_DIR}
BENCH_DEPS=("${BIN}" "${INPUT}" \
    "${BIN_WORK_DIR}/diskann-benchmark/tests" \
    "${BIN_WORK_DIR}/diskann-benchmark/tests.data")
BENCH_CATEGORY="ann"
BENCH_DESCRIPTION="DiskANN in-memory graph load and approximate-nearest-neighbor search"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

export BIN_WORK_DIR
export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

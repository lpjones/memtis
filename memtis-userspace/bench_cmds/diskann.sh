#!/bin/bash

BIN_WORK_DIR=/proj/TppPlus/tpp/workloads/DiskANN
BENCH_RUN="cargo run --release --package diskann-benchmark -- run \
    --input-file /proj/TppPlus/tpp/workloads/DiskANN/diskann-benchmark/example/load.json \
    --output-file dynamic-output.json"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

export BIN_WORK_DIR
export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

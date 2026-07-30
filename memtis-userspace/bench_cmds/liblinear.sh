#!/bin/bash

BENCH_BIN=/proj/TppPlus/tpp/workloads/liblinear
DATASET=/mnt/pact_storage/workloads/liblinear/data_50m.bin

# data_large.bin: 30M samples x 1M features, 20 nnz/sample (~7.7GB on disk)
# solve-time anon footprint ~21GB (row + column copies of the problem)
# generated with: gen_data.py --samples 30000000 --features 1000000 --nnz_per_sample 20
# NOTE: local train binary is single-core (no -m); -e 0.1 bounds runtime
BENCH_RUN=("${BENCH_BIN}/train" -s 6 -e 0.1 "${DATASET}")
BENCH_DEPS=("${BENCH_BIN}/train" "${DATASET}")
BENCH_CATEGORY="machine-learning"
BENCH_DESCRIPTION="Liblinear L1-regularized logistic regression, 50M sparse samples"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="1300MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="2400MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="4300MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="7200MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="10600MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="25000MB"
fi

export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

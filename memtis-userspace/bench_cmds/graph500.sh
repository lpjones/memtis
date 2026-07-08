#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/graph500-v2/omp-csr

# -s 26 -e 15 => ~25GB peak on this testbed (-s 27 => ~41GB, too large)
# validation is skipped (single-threaded and extremely slow at this scale)
BENCH_RUN="${BIN}/omp-csr -s 26 -e 15 -V"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"
export SKIP_VALIDATION=1

if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="1500MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="2800MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="5100MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="8500MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="12700MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="28000MB"
fi

export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

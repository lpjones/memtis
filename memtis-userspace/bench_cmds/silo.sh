#!/bin/bash

BIN=/proj/TppPlus/tpp/memtis/memtis-userspace/bench_dir/silo/out-pagr.masstree/benchmarks

# scale-factor 130000 => ~19GB memory footprint (400000 => ~59.5GB)
# sized for the 48GB pc800 memtis testbed
# NOTE: dbtest asserts if both --runtime and --ops-per-worker are given
BENCH_RUN=("${BIN}/dbtest" --verbose --bench ycsb --num-threads 16 \
    --scale-factor 240000 --runtime 300 --slow-exit)
BENCH_DEPS=("${BIN}/dbtest")
BENCH_CATEGORY="database"
BENCH_DESCRIPTION="Silo YCSB database workload at scale factor 240,000"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="600MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="1150MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="2150MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="3900MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="6450MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="9700MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="23000MB"
fi

export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

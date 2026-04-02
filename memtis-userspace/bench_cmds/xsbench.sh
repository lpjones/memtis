#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/XSBench/openmp-threading
BENCH_RUN="${BIN}/XSBench -t 8 -g 75000 -p 5000000"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

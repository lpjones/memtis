#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/llama
BENCH_RUN="${BIN}/benchmark-matmult -t 8 -i 10"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

#!/bin/bash

BIN=/proj/TppPlus/tpp/workloads/stream
BENCH_RUN="${BIN}/stream 12288 50"
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

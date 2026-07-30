#!/bin/bash

BIN=/proj/TppPlus/tpp/scripts/venv/bin/python
SCRIPT=/proj/TppPlus/tpp/workloads/resnet/resnet_train.py
BENCH_RUN=("${BIN}" "${SCRIPT}")
BENCH_DEPS=("${BIN}" "${SCRIPT}")
BENCH_CATEGORY="machine-learning"
BENCH_DESCRIPTION="CPU MnasNet training with batch size 768"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

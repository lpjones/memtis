#!/bin/bash

DLRM_DIR=/mnt/pact_storage/workloads/dlrm
PYTHON=${DLRM_DIR}/dlrm_env/bin/python
SCRIPT=${DLRM_DIR}/dlrm_s_pytorch.py
EMBEDDINGS=10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000-10000000

BENCH_RUN=("${PYTHON}" "${SCRIPT}" --data-generation=random --inference-only \
    --arch-sparse-feature-size=64 --arch-embedding-size="${EMBEDDINGS}" \
    --arch-mlp-bot=13-512-256-64 --arch-mlp-top=512-256-1 \
    --mini-batch-size=1024 --num-batches=10)
BENCH_CWD=${DLRM_DIR}
BENCH_DEPS=("${PYTHON}" "${SCRIPT}")
BENCH_CATEGORY="recommendation"
BENCH_DESCRIPTION="DLRM inference with fifteen 10M-row, 64-float embedding tables"
BENCH_MEMORY_MIN_GB=32
BENCH_MEMORY_MAX_GB=48
BENCH_SLOW_MIN_GB=1
BENCH_DRAM="max"
BENCH_DRAM_BUFFER_MB="1024"


export BENCH_RUN
export BENCH_DRAM
export BENCH_DRAM_BUFFER_MB

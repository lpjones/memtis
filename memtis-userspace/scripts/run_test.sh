#!/bin/bash
py_bin="/proj/TppPlus/tpp/scripts/venv/bin/python"

./run_bench.sh --cxl -B cgups
# ./run_bench.sh --cxl -B resnet
# ./run_bench.sh --cxl -B stream
# ./run_bench.sh --cxl -B xsbench

# export OMP_NUM_THREADS=8

# ./run_bench.sh --cxl -B nas-bt
# ./run_bench.sh --cxl -B nas-cg
# ./run_bench.sh --cxl -B nas-ft
# ./run_bench.sh --cxl -B nas-is
# ./run_bench.sh --cxl -B nas-lu
# ./run_bench.sh --cxl -B nas-mg
# ./run_bench.sh --cxl -B nas-sp
$py_bin ../plot_metrics.py ../results/cgups/dmesg.txt --title "MEMTIS GUPS Metrics"


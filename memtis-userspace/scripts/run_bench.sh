#!/bin/bash

set -o pipefail

if [[ -z "${NTHREADS:-}" ]]; then
    NTHREADS=$(grep -c processor /proc/cpuinfo)
fi
export NTHREADS
NCPU_NODES=$(cat /sys/devices/system/node/has_cpu | awk -F '-' '{print $NF+1}')
NMEM_NODES=$(cat /sys/devices/system/node/has_memory | awk -F '-' '{print $NF+1}')
MEM_NODES=($(ls /sys/devices/system/node | grep node | awk -F 'node' '{print $NF}'))

CGROUP_NAME="htmm"
###### update DIR!
DIR=/proj/TppPlus/tpp/memtis/memtis-userspace
plot_dir=/proj/TppPlus/tpp/scripts/plot_scripts
py_bin=/proj/TppPlus/tpp/scripts/venv/bin/python
RESULTS_DIR=${RESULTS_DIR:-${DIR}/results}
BENCHMARK_TIMEOUT=${BENCHMARK_TIMEOUT:-1h}
SAMPLER_CLEANUP_TIMEOUT=${SAMPLER_CLEANUP_TIMEOUT:-30s}

CONFIG_PERF=off
CONFIG_NS=off
CONFIG_NW=off
CONFIG_CXL_MODE=off
CONFIG_CHECK_ONLY=off
STATIC_DRAM=""
DATE=""
VER=""
NVM_RATIO=""
BENCH_RC=0
declare -a BENCH_CMD=()

function func_cache_flush() {
    echo 3 > /proc/sys/vm/drop_caches
	sync
    free
    return
}

function set_htmm_knob() {
    local name="$1"
    local value="$2"
    local path="/sys/kernel/mm/htmm/${name}"

    if [[ -w "${path}" ]]; then
	echo "${value}" | tee "${path}"
    else
	echo "WARNING: ${path} is not writable or does not exist; wanted ${value}"
    fi
}

function func_memtis_setting() {
    echo 199 | tee /sys/kernel/mm/htmm/htmm_sample_period
    echo 100007 | tee /sys/kernel/mm/htmm/htmm_inst_sample_period
    echo 1 | tee /sys/kernel/mm/htmm/htmm_thres_hot
    echo 2 | tee /sys/kernel/mm/htmm/htmm_split_period
    echo 100000 | tee /sys/kernel/mm/htmm/htmm_adaptation_period
    echo 2000000 | tee /sys/kernel/mm/htmm/htmm_cooling_period
    echo 2 | tee /sys/kernel/mm/htmm/htmm_mode
    echo 500 | tee /sys/kernel/mm/htmm/htmm_demotion_period_in_ms
    echo 500 | tee /sys/kernel/mm/htmm/htmm_promotion_period_in_ms
    echo 4 | tee /sys/kernel/mm/htmm/htmm_gamma
    ###  cpu cap (per mille) for ksampled
    echo 30 | tee /sys/kernel/mm/htmm/ksampled_soft_cpu_quota

    if [[ "x${CONFIG_NS}" == "xoff" ]]; then
	echo 1 | tee /sys/kernel/mm/htmm/htmm_thres_split
    else
	echo 0 | tee /sys/kernel/mm/htmm/htmm_thres_split
    fi

    if [[ "x${CONFIG_NW}" == "xoff" ]]; then
	echo 0 | tee /sys/kernel/mm/htmm/htmm_nowarm
    else
	echo 1 | tee /sys/kernel/mm/htmm/htmm_nowarm
    fi

    if [[ "x${CONFIG_CXL_MODE}" == "xon" ]]; then
	${DIR}/scripts/set_uncore_freq.sh on
	echo "enabled" | tee /sys/kernel/mm/htmm/htmm_cxl_mode
    else
	${DIR}/scripts/set_uncore_freq.sh off
	echo "disabled" | tee /sys/kernel/mm/htmm/htmm_cxl_mode
    fi

    echo "always" | tee /sys/kernel/mm/transparent_hugepage/enabled
    echo "always" | tee /sys/kernel/mm/transparent_hugepage/defrag

    # PAGR tuning knobs. Defaults keep comparison traces enabled while reducing
    # prediction fanout and graph/log overhead relative to the first port.
    set_htmm_knob pagr_fast_threshold_min_percent "${PAGR_FAST_THRESHOLD_MIN_PERCENT:-5}"
    set_htmm_knob pagr_fast_threshold_power "${PAGR_FAST_THRESHOLD_POWER:-2}"
    set_htmm_knob pagr_fast_threshold_min_samples "${PAGR_FAST_THRESHOLD_MIN_SAMPLES:-1024}"
    set_htmm_knob pagr_max_predictions_per_sample "${PAGR_MAX_PREDICTIONS_PER_SAMPLE:-4}"
    set_htmm_knob pagr_trace_enabled "${PAGR_TRACE:-1}"
    set_htmm_knob pagr_graph_enabled "${PAGR_GRAPH:-1}"
    set_htmm_knob pagr_graph_sample_interval "${PAGR_GRAPH_SAMPLE_INTERVAL:-8}"
    set_htmm_knob pagr_debug_interval_ms "${PAGR_DEBUG_INTERVAL_MS:-0}"
    set_htmm_knob pagr_verbose "${PAGR_VERBOSE:-0}"
}

function func_load_benchmark() {
    local config="${DIR}/bench_cmds/${BENCH_NAME}.sh"
    local dep
    local command_decl

    export BENCH_NAME
    export NVM_RATIO
    if [[ "${NVM_RATIO}" == "static" && -n "${STATIC_DRAM}" ]]; then
        export STATIC_DRAM
    fi

    if [[ ! -f "${config}" ]]; then
        echo "ERROR: benchmark config does not exist: ${config}" >&2
        return 1
    fi

    unset BENCH_RUN BENCH_CWD BENCH_DEPS BENCH_CATEGORY BENCH_DESCRIPTION
    unset BENCH_MEMORY_MIN_GB BENCH_MEMORY_MAX_GB BENCH_SLOW_MIN_GB
    unset BENCH_HTMM_NOPID BENCH_DRAM BENCH_DRAM_BUFFER_MB
    BENCH_DEPS=()
    # shellcheck disable=SC1090
    source "${config}"

    command_decl=$(declare -p BENCH_RUN 2>/dev/null) || {
        echo "ERROR: ${config} did not define BENCH_RUN" >&2
        return 1
    }
    if [[ "${command_decl}" == "declare -a"* ]]; then
        BENCH_CMD=("${BENCH_RUN[@]}")
    else
        read -r -a BENCH_CMD <<< "${BENCH_RUN}"
    fi

    BENCH_CWD=${BENCH_CWD:-${DIR}}
    BENCH_CATEGORY=${BENCH_CATEGORY:-uncategorized}
    BENCH_DESCRIPTION=${BENCH_DESCRIPTION:-${BENCH_NAME}}
    BENCH_MEMORY_MIN_GB=${BENCH_MEMORY_MIN_GB:-32}
    BENCH_MEMORY_MAX_GB=${BENCH_MEMORY_MAX_GB:-48}
    BENCH_SLOW_MIN_GB=${BENCH_SLOW_MIN_GB:-1}
    BENCH_HTMM_NOPID=${BENCH_HTMM_NOPID:-0}

    if [[ ! -d "${BENCH_CWD}" ]]; then
        echo "ERROR: benchmark working directory does not exist: ${BENCH_CWD}" >&2
        return 1
    fi
    if [[ ${#BENCH_CMD[@]} -eq 0 ]]; then
        echo "ERROR: ${config} defined an empty BENCH_RUN" >&2
        return 1
    fi
    if ! command -v "${BENCH_CMD[0]}" >/dev/null 2>&1; then
        echo "ERROR: benchmark executable is missing: ${BENCH_CMD[0]}" >&2
        return 1
    fi
    for dep in "${BENCH_DEPS[@]}"; do
        if [[ ! -e "${dep}" ]]; then
            echo "ERROR: ${BENCH_NAME} dependency is missing: ${dep}" >&2
            return 1
        fi
    done

    printf 'benchmark: %s\ncategory: %s\ndescription: %s\n' \
        "${BENCH_NAME}" "${BENCH_CATEGORY}" "${BENCH_DESCRIPTION}"
    printf 'expected memory: %s-%s GiB; minimum slow-tier use: %s GiB\n' \
        "${BENCH_MEMORY_MIN_GB}" "${BENCH_MEMORY_MAX_GB}" "${BENCH_SLOW_MIN_GB}"
    printf 'working directory: %s\ncommand:' "${BENCH_CWD}"
    printf ' %q' "${BENCH_CMD[@]}"
    printf '\n'
}

function func_prepare() {
    echo "Preparing benchmark start..."

	sudo sysctl kernel.perf_event_max_sample_rate=100000

	# disable automatic numa balancing
	sudo echo 0 > /proc/sys/kernel/numa_balancing
	# set configs
	func_memtis_setting
	
	DATE=$(date +%Y%m%d%H%M)
}

function peak_slow_tier_bytes() {
    awk '
        /^[0-9]+$/ { current = 0; next }
        $1 == "anon" || $1 == "file" {
            for (i = 2; i <= NF; i++) {
                if ($i ~ /^N1=/) {
                    split($i, value, "=")
                    current += value[2]
                }
            }
            if ($1 == "file" && current > peak)
                peak = current
        }
        END { print peak + 0 }
    ' "$1"
}

function validate_memory_range() {
    local memory_log="$1"
    local numa_log="$2"
    local summary="$3"
    local peak_bytes peak_slow_bytes
    local min_bytes max_bytes min_slow_bytes

    peak_bytes=$(awk '{ if ($2 > peak) peak = $2 } END { print peak + 0 }' "${memory_log}")
    peak_slow_bytes=$(peak_slow_tier_bytes "${numa_log}")
    min_bytes=$((BENCH_MEMORY_MIN_GB * 1024 * 1024 * 1024))
    max_bytes=$((BENCH_MEMORY_MAX_GB * 1024 * 1024 * 1024))
    min_slow_bytes=$((BENCH_SLOW_MIN_GB * 1024 * 1024 * 1024))

    {
        printf 'metric\tbytes\tGiB\n'
        awk -v value="${peak_bytes}" 'BEGIN { printf "peak_cgroup_memory\t%.0f\t%.2f\n", value, value / 1073741824 }'
        awk -v value="${peak_slow_bytes}" 'BEGIN { printf "peak_slow_tier_memory\t%.0f\t%.2f\n", value, value / 1073741824 }'
    } > "${summary}"

    awk -v peak="${peak_bytes}" -v slow="${peak_slow_bytes}" \
        'BEGIN { printf "Measured peak: %.2f GiB total, %.2f GiB on node1\n", peak / 1073741824, slow / 1073741824 }'

    if (( peak_bytes < min_bytes || peak_bytes > max_bytes )); then
        echo "ERROR: ${BENCH_NAME} peak memory is outside ${BENCH_MEMORY_MIN_GB}-${BENCH_MEMORY_MAX_GB} GiB" >&2
        return 1
    fi
    if (( peak_slow_bytes < min_slow_bytes )); then
        echo "ERROR: ${BENCH_NAME} used less than ${BENCH_SLOW_MIN_GB} GiB on the slow tier" >&2
        return 1
    fi
}

function func_main() {
    local launcher="${DIR}/bin/launch_bench"
    local memory_monitor_pid dmesg_pid benchmark_rc cleanup_rc
    local max_dram_size bench_dram_upper req_dram_mb dram_limit_to_set
    local benchmark_cpus=16,18,20,22,24,26,28,30
    local -a pinning=(numactl --physcpubind="${benchmark_cpus}")

    timeout --verbose --signal=TERM --kill-after=5s \
	"${SAMPLER_CLEANUP_TIMEOUT}" "${DIR}/bin/kill_ksampled"
    cleanup_rc=$?
    if (( cleanup_rc != 0 )); then
	echo "ERROR: could not stop the previous memory sampler (rc=${cleanup_rc}); refusing to start ${BENCH_NAME}" >&2
	return 125
    fi
    TIME="/usr/bin/time"

    echo "-----------------------"
    echo "NVM RATIO: ${NVM_RATIO}"
    echo "${DATE}"
    echo "Pinned CPUs: ${benchmark_cpus}"
    echo "Benchmark timeout: ${BENCHMARK_TIMEOUT}"
    echo "-----------------------"

    rm -rf "${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}"
    mkdir -p "${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}"
    LOG_DIR="${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}"

    func_cache_flush

    max_dram_size=$(numastat -m | awk '$1 == "MemFree" { print int($2) }')
    bench_dram_upper=${BENCH_DRAM^^}
    req_dram_mb=""
    dram_limit_to_set=""

    if [[ "${BENCH_DRAM,,}" == "max" ]]; then
        BENCH_DRAM_BUFFER_MB=${BENCH_DRAM_BUFFER_MB:-1024}
        req_dram_mb=$((max_dram_size - BENCH_DRAM_BUFFER_MB))
        if (( req_dram_mb < 1024 )); then
            echo "ERROR: node0 has ${max_dram_size} MiB free, less than the ${BENCH_DRAM_BUFFER_MB} MiB buffer" >&2
            return 1
        fi
        dram_limit_to_set="${req_dram_mb}MB"
        echo "Using auto DRAM budget on node0: ${dram_limit_to_set} (MemFree=${max_dram_size}MB, buffer=${BENCH_DRAM_BUFFER_MB}MB)"
    else
        if [[ ${bench_dram_upper} =~ ^([0-9]+)MB$ ]]; then
            req_dram_mb=${BASH_REMATCH[1]}
        elif [[ ${bench_dram_upper} =~ ^([0-9]+)GB$ ]]; then
            req_dram_mb=$((BASH_REMATCH[1] * 1024))
        else
            echo "ERROR: BENCH_DRAM must be [NMB], [NGB], or max" >&2
            return 1
        fi
        if (( req_dram_mb > max_dram_size )); then
            echo "ERROR: node0 has only ${max_dram_size} MiB free; ${BENCH_NAME} requested ${req_dram_mb} MiB" >&2
            return 1
        fi
        dram_limit_to_set="${bench_dram_upper}"
    fi

    sudo "${DIR}/scripts/set_htmm_memcg.sh" htmm remove
    sudo "${DIR}/scripts/set_htmm_memcg.sh" htmm $$ enable
    sudo "${DIR}/scripts/set_mem_size.sh" htmm 0 "${dram_limit_to_set}"
    sleep 2

    grep -e thp -e htmm -e pgmig /proc/vmstat > "${LOG_DIR}/before_vmstat.log"
    func_cache_flush
    sleep 2

    sudo dmesg -c > /dev/null
    sudo stdbuf -oL dmesg -w > "${LOG_DIR}/dmesg.txt" &
    dmesg_pid=$!

    "${DIR}/scripts/memory_stat.sh" "${LOG_DIR}" &
    memory_monitor_pid=$!
    if [[ "${BENCH_HTMM_NOPID}" -eq 1 ]]; then
        launcher="${DIR}/bin/launch_bench_nopid"
    fi

    if [[ -n "${BENCH_ARG:-}" ]]; then
        (
            cd "${BENCH_CWD}" || exit 125
            "${TIME}" -f "execution time %e (s)" \
                timeout --verbose --signal=TERM --kill-after=30s \
                    "${BENCHMARK_TIMEOUT}" \
                    "${pinning[@]}" "${launcher}" "${BENCH_CMD[@]}" < "${BENCH_ARG}"
        ) 2>&1 | tee "${LOG_DIR}/output.log"
    else
        (
            cd "${BENCH_CWD}" || exit 125
            "${TIME}" -f "execution time %e (s)" \
                timeout --verbose --signal=TERM --kill-after=30s \
                    "${BENCHMARK_TIMEOUT}" \
                    "${pinning[@]}" "${launcher}" "${BENCH_CMD[@]}"
        ) 2>&1 | tee "${LOG_DIR}/output.log"
    fi
    benchmark_rc=${PIPESTATUS[0]}
    if ((benchmark_rc == 124 || benchmark_rc == 137)) && \
        grep -q '^timeout: sending signal' "${LOG_DIR}/output.log"; then
        benchmark_rc=124
        echo "ERROR: benchmark exceeded ${BENCHMARK_TIMEOUT} and was terminated" \
            | tee -a "${LOG_DIR}/output.log" >&2
    fi

    kill "${memory_monitor_pid}" 2>/dev/null || true
    wait "${memory_monitor_pid}" 2>/dev/null || true
    grep -e thp -e htmm -e pgmig /proc/vmstat > "${LOG_DIR}/after_vmstat.log"
    sleep 2

    if [[ "${BENCH_NAME}" == "btree" ]]; then
        grep Throughput "${LOG_DIR}/output.log" \
            | awk 'NR%20==0 { print sum; sum = 0; next } { sum += $3 }' \
            > "${LOG_DIR}/throughput.out" || true
    elif [[ "${BENCH_NAME}" == "silo" ]]; then
        grep -e '0 throughput' -e '5 throughput' "${LOG_DIR}/output.log" \
            | awk '{ print $4 }' > "${LOG_DIR}/throughput.out" || true
    fi

    sudo kill "${dmesg_pid}" 2>/dev/null || true
    wait "${dmesg_pid}" 2>/dev/null || true
    sudo dmesg -c > /dev/null
    sudo "${DIR}/scripts/set_htmm_memcg.sh" htmm $$ disable

    if (( benchmark_rc != 0 )); then
        BENCH_RC=${benchmark_rc}
    elif ! validate_memory_range "${LOG_DIR}/memory_current.txt" \
        "${LOG_DIR}/memory_numa_stat.txt" "${LOG_DIR}/memory_summary.tsv"; then
        BENCH_RC=86
    fi

    return "${BENCH_RC}"
}

function func_usage() {
    echo
    echo -e "Usage: $0 [-b benchmark name] [-s socket_mode] [-w GB] ..."
    echo
    echo "  -B,   --benchmark   [arg]    benchmark name to run. e.g., graph500, Liblinear, etc"
    echo "  -R,   --ratio       [arg]    fast tier size vs. capacity tier size: \"1:16\", \"1:8\", or \"1:2\""
    echo "  -D,   --dram        [arg]    static dram size [MB or GB]; only available when -R is set to \"static\""
    echo "  -V,   --version     [arg]    a version name for results"
    echo "  -NS,  --nosplit              disable skewness-aware page size determination"
    echo "  -NW,  --nowarm               disable the warm set"
    echo "        --cxl                  enable cxl mode [default: disabled]"
    echo "        --check-only           validate the workload command and dependencies"
    echo "  -?,   --help"
    echo "        --usage"
    echo
}


################################ Main ##################################

if [ "$#" == 0 ]; then
    echo "Error: no arguments"
    func_usage
    exit -1
fi

# get options:
while (( "$#" )); do
    case "$1" in
	-B|--benchmark)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		BENCH_NAME=( "$2" )
		shift 2
	    else
		echo "Error: Argument for $1 is missing" >&2
		func_usage
		exit -1
	    fi
	    ;;
	-V|--version)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		VER=( "$2" )
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-P|--perf)
	    CONFIG_PERF=on
	    shift 1
	    ;;
	-R|--ratio)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		NVM_RATIO="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-D|--dram)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		STATIC_DRAM="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-NS|--nosplit)
	    CONFIG_NS=on
	    shift 1
	    ;;
	-NW|--nowarm)
	    CONFIG_NW=on
	    shift 1
	    ;;
	--cxl)
	    CONFIG_CXL_MODE=on
	    shift 1
	    ;;
	--check-only)
	    CONFIG_CHECK_ONLY=on
	    shift 1
	    ;;
	-H|-?|-h|--help|--usage)
	    func_usage
	    exit
	    ;;
	*)
	    echo "Error: Invalid option $1"
	    func_usage
	    exit -1
	    ;;
    esac
done

if [ -z "${BENCH_NAME}" ]; then
    echo "Benchmark name must be specified"
    func_usage
    exit -1
fi

function func_plot() {
	local app_dir=${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}/

	$py_bin ${DIR}/plot_metrics.py \
		${app_dir}dmesg.txt \
		--pgmig ${app_dir}pgmig.txt \
		--output-dir ${app_dir} \
		--title "${BENCH_NAME} ${VER} ${NVM_RATIO}" \
		|| echo "WARNING: failed to plot dmesg/pgmig statistics"

	# per-node memory allocation over time (PACT-style mem plot)
	if [[ -f ${app_dir}numactl.txt ]]; then
		$py_bin $plot_dir/plot_numactl.py \
			${app_dir}numactl.txt \
			${app_dir}mem.png \
			--labels "node 0 free" "node 1 free" \
			--title "${BENCH_NAME} ${VER} ${NVM_RATIO} Free Mem" \
			|| echo "WARNING: failed to plot numactl statistics"
	fi

	[[ -f /tmp/memtis_pebs_trace.bin ]] && mv /tmp/memtis_pebs_trace.bin ${app_dir}
	[[ -f /tmp/memtis_pred.bin ]] && mv /tmp/memtis_pred.bin ${app_dir}
	[[ -f /tmp/memtis_promote.bin ]] && mv /tmp/memtis_promote.bin ${app_dir}
	[[ -f /tmp/memtis_demote.bin ]] && mv /tmp/memtis_demote.bin ${app_dir}

	if [[ -f /tmp/memtis_pagr_graph.bin ]]; then
		mv /tmp/memtis_pagr_graph.bin ${app_dir}
		$py_bin ${DIR}/plot_pagr_graph.py \
			${app_dir}memtis_pagr_graph.bin \
			--output ${app_dir}pagr_graph.png \
			--summary ${app_dir}pagr_graph_edges.csv \
			--title "${BENCH_NAME} ${VER} ${NVM_RATIO} PAGR graph" \
			|| echo "WARNING: failed to plot PAGR graph"
	fi
	

	if [[ $BENCH_NAME == "cgups" ]]; then
		$py_bin $plot_dir/plot_cgups_mul.py \
			${app_dir}output.log \
			${app_dir}cgups_plot.png
	fi

	rm -rf ${app_dir}pred_acc.txt

	if [[ -f ${app_dir}memtis_pebs_trace.bin ]]; then
		$py_bin $plot_dir/plot_cluster_no_app.py \
			${app_dir}memtis_pebs_trace.bin \
			--output ${app_dir}plot.png \
			-fast
	fi

	if [[ -f ${app_dir}memtis_pred.bin ]]; then
		$py_bin $plot_dir/plot_cluster_no_app.py \
			${app_dir}memtis_pred.bin \
			--output ${app_dir}pred_plot.png \
			-fast
	fi

	if [[ -f ${app_dir}memtis_promote.bin ]]; then
		$py_bin $plot_dir/plot_cluster_no_app.py \
			${app_dir}memtis_promote.bin \
			--output ${app_dir}promote_plot.png \
			-fast
	fi

	if [[ -f ${app_dir}memtis_demote.bin ]]; then
		$py_bin $plot_dir/plot_cluster_no_app.py \
			${app_dir}memtis_demote.bin \
			--output ${app_dir}demote_plot.png \
			-fast
	fi

	if [[ -f ${app_dir}memtis_pebs_trace.bin && \
	      -f ${app_dir}memtis_promote.bin && \
	      -f ${app_dir}memtis_demote.bin ]]; then
		$py_bin "${plot_dir}/pred_acc.py" \
		"${app_dir}/memtis_pebs_trace.bin" \
		"${app_dir}/memtis_promote.bin" \
		"${app_dir}/memtis_demote.bin" \
		>> "${app_dir}/pred_acc.txt"


		$py_bin "${plot_dir}/plot_timeliness.py" \
		"${app_dir}/memtis_pebs_trace.bin" \
		"${app_dir}/memtis_promote.bin" \
		"${app_dir}/memtis_demote.bin" \
		--output "${app_dir}/timeliness.png" \
		>> "${app_dir}/pred_acc.txt"

		$py_bin "${plot_dir}/cost_benefit.py" \
		"${app_dir}/memtis_pebs_trace.bin" \
		"${app_dir}/memtis_promote.bin" \
		"${app_dir}/memtis_demote.bin" \
		--output "${app_dir}/cost_benefit.png" \
		>> "${app_dir}/pred_acc.txt"
	fi

	# $py_bin "${plot_dir}/plot_pred_acc.py" \
    #     ${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}/memtis_pebs_trace.bin \
    #     ${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}/memtis_pred.bin \
    #     --output ${RESULTS_DIR}/${BENCH_NAME}/${VER}/${NVM_RATIO}/pred.png \
    #     --tw 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1
}

if ! func_load_benchmark; then
    exit 2
fi
if [[ "${CONFIG_CHECK_ONLY}" == "on" ]]; then
    exit 0
fi

func_prepare
func_main
BENCH_RC=$?
func_plot
exit "${BENCH_RC}"

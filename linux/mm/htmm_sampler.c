/*
 * memory access sampling for hugepage-aware tiered memory management.
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>
#include <linux/cpumask.h>
#include <linux/numa.h>
#include <linux/err.h>
#include <linux/vmstat.h>
#include <linux/printk.h>

#include "../kernel/events/internal.h"

#include <linux/htmm.h>

static struct file *memtis_trace_file;
static loff_t memtis_trace_pos;

struct pebs_rec {
  uint64_t cyc;
  uint64_t va;
  uint64_t ip;
  uint32_t cpu;
  uint8_t  evt;
} __attribute__((packed));

struct task_struct *access_sampling = NULL;
struct perf_event ***mem_event;
static struct file ***mem_event_files;
static int htmm_target_node;

static void htmm_release_mem_events(void)
{
	int cpu, event;

	if (!mem_event)
		return;

	for_each_possible_cpu(cpu) {
		if (mem_event_files && mem_event_files[cpu]) {
			for (event = 0; event < N_HTMMEVENTS; event++) {
				if (mem_event_files[cpu][event]) {
					fput(mem_event_files[cpu][event]);
					mem_event_files[cpu][event] = NULL;
				}
			}
			kfree(mem_event_files[cpu]);
		}

		kfree(mem_event[cpu]);
	}

	kfree(mem_event_files);
	mem_event_files = NULL;
	kfree(mem_event);
	mem_event = NULL;
}

static const struct cpumask *htmm_sample_cpumask(void)
{
	if (htmm_target_node >= 0 && htmm_target_node < nr_node_ids &&
	    !cpumask_empty(cpumask_of_node(htmm_target_node)))
		return cpumask_of_node(htmm_target_node);

	return cpu_online_mask;
}

static inline bool htmm_perf_rb_copy(struct perf_buffer *rb, u64 offset,
				     void *dst, size_t size)
{
	unsigned long page_sz;
	unsigned long pg_shift;
	size_t copied = 0;

	if (!rb || !dst || !size || !rb->nr_pages) {
		printk("invalid argument(s) for htmm_perf_rb_copy\n");
		return false;
	}

	pg_shift = PAGE_SHIFT + page_order(rb);
	page_sz = 1UL << pg_shift;

	while (copied < size) {
		u64 cur = offset + copied;
		unsigned long pg = (cur >> pg_shift) % rb->nr_pages;
		unsigned long in_page = cur & (page_sz - 1);
		size_t chunk = min_t(size_t, size - copied, page_sz - in_page);

		memcpy((char *)dst + copied, rb->data_pages[pg] + in_page, chunk);
		copied += chunk;
	}

	return true;
}

static bool valid_va(unsigned long addr)
{
	if (!(addr >> (PGDIR_SHIFT + 9)) && addr != 0)
	return true;

	return false;
}

static __u64 get_pebs_event(enum events e)
{
    switch (e) {
	case DRAMREAD:
	    return DRAM_LLC_LOAD_MISS;
	case NVMREAD:
	    if (!htmm_cxl_mode)
		return NVM_LLC_LOAD_MISS;
	    else
		return N_HTMMEVENTS;
	case MEMWRITE:
	    return ALL_STORES;
	case CXLREAD:
	    if (htmm_cxl_mode)
		return REMOTE_DRAM_LLC_LOAD_MISS;
	    else
		return N_HTMMEVENTS;
	default:
	    return N_HTMMEVENTS;
    }
}

static int __perf_event_open(__u64 config, __u64 config1, __u64 cpu,
	__u64 type, __u32 pid)
{
    struct perf_event_attr attr = {0};
    struct file *file;
    int event_fd, __pid;

    attr.type = PERF_TYPE_RAW;
    attr.size = sizeof(struct perf_event_attr);
    attr.config = config;
    attr.config1 = config1;
    if (config == ALL_STORES)
	attr.sample_period = htmm_inst_sample_period;
    else
	attr.sample_period = get_sample_period(0);
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
    attr.disabled = 0;
	attr.inherit = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 1;
    attr.precise_ip = 1;

    // if (pid == 0)
	// __pid = -1;
    // else
	// __pid = pid;
	__pid = -1; // for system-wide sampling, set pid to -1 to capture events from all processes

	printk("pid: %d, cpu: %llu, event: %llu\n", __pid, cpu, config);
	
    event_fd = htmm__perf_event_open(&attr, __pid, cpu, -1, 0);
    //event_fd = htmm__perf_event_open(&attr, -1, cpu, -1, 0);
    if (event_fd < 0) {
	printk("[error htmm__perf_event_open failure] event_fd: %d\n", event_fd);
	return -1;
    }

    file = fget(event_fd);
    if (!file) {
	printk("invalid file\n");
	close_fd(event_fd);
	return -1;
    }
	close_fd(event_fd);

	if (!mem_event_files || !mem_event_files[cpu]) {
	fput(file);
	return -ENOMEM;
	}

    mem_event[cpu][type] = file->private_data;
	mem_event_files[cpu][type] = file;
    printk("[pebs_open] cpu=%llu, event=%llu, perf_event=%p, state=%d, period=%lld\n",
    	   cpu, config, mem_event[cpu][type],
    	   mem_event[cpu][type]->state,
	   local64_read(&mem_event[cpu][type]->hw.period_left));
    return 0;
}

static int pebs_init(pid_t pid, int node)
{
    int cpu, event;
    const struct cpumask *sample_mask;

    htmm_target_node = node;
    sample_mask = htmm_sample_cpumask();

    mem_event = kcalloc(nr_cpu_ids, sizeof(*mem_event), GFP_KERNEL);
    if (!mem_event)
	return -ENOMEM;

    mem_event_files = kcalloc(nr_cpu_ids, sizeof(*mem_event_files), GFP_KERNEL);
    if (!mem_event_files) {
	kfree(mem_event);
	mem_event = NULL;
	return -ENOMEM;
	}

    for_each_possible_cpu(cpu) {
	mem_event[cpu] = kcalloc(N_HTMMEVENTS, sizeof(*mem_event[cpu]), GFP_KERNEL);
	mem_event_files[cpu] = kcalloc(N_HTMMEVENTS, sizeof(*mem_event_files[cpu]), GFP_KERNEL);
	if (!mem_event[cpu] || !mem_event_files[cpu]) {
	    htmm_release_mem_events();
	    return -ENOMEM;
	}
    }

    printk("pebs_init\n");

    for_each_cpu(cpu, sample_mask) {
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    if (get_pebs_event(event) == N_HTMMEVENTS) {
		mem_event[cpu][event] = NULL;
		continue;
	    }

	    if (__perf_event_open(get_pebs_event(event), 0, cpu, event, pid))
		goto out_err;
	    if (htmm__perf_event_init(mem_event[cpu][event], BUFFER_SIZE))
		goto out_err;
	}
    }

    return 0;

out_err:
	htmm_release_mem_events();
    return -1;
}

static void pebs_disable(void)
{
    int cpu, event;
    const struct cpumask *sample_mask = htmm_sample_cpumask();

    printk("pebs disable\n");
	if (!mem_event)
		return;

    for_each_cpu(cpu, sample_mask) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (mem_event[cpu] && mem_event[cpu][event])
				perf_event_disable(mem_event[cpu][event]);
		}
    }
	if (memtis_trace_file) {
		filp_close(memtis_trace_file, NULL);
		memtis_trace_file = NULL;
		memtis_trace_pos = 0;
	}
	htmm_pred_log_stop();
	htmm_release_mem_events();
}

static void pebs_enable(void)
{
    int cpu, event;
	const struct cpumask *sample_mask = htmm_sample_cpumask();
	int enabled_count = 0;

    printk("pebs enable\n");
    if (!mem_event)
	return;

	memtis_trace_file = filp_open("/tmp/memtis_pebs_trace.bin",
				     O_WRONLY | O_CREAT | O_TRUNC,
				     0600);
	if (IS_ERR(memtis_trace_file)) {
		printk("failed to open memtis trace file: %ld\n",
		       PTR_ERR(memtis_trace_file));
		memtis_trace_file = NULL;
		return;
	}
	memtis_trace_pos = 0;
	if (htmm_pred_log_start())
		pr_warn("htmm: prediction logging disabled\n");

	for_each_cpu(cpu, sample_mask) {
	if (!mem_event[cpu])
	    continue;
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    if (mem_event[cpu][event]) {
	    	struct perf_event *e = mem_event[cpu][event];
	    	printk("[enable_evt] cpu=%d event=%d config=%llu state_before=%d\n",
	    	       cpu, event, e->attr.config, e->state);
		perf_event_enable(mem_event[cpu][event]);
		enabled_count++;
	    }
	}
    }
    printk("[enable_done] total_enabled=%d\n", enabled_count);
}

static void pebs_update_period(uint64_t value, uint64_t inst_value)
{
    int cpu, event;
    const struct cpumask *sample_mask = htmm_sample_cpumask();

    if (!mem_event)
	return;

    for_each_cpu(cpu, sample_mask) {
	if (!mem_event[cpu])
	    continue;
	for (event = 0; event < N_HTMMEVENTS; event++) {
	    int ret;
	    if (!mem_event[cpu][event])
		continue;

	    switch (event) {
		case DRAMREAD:
		case NVMREAD:
		case CXLREAD:
		    ret = perf_event_period(mem_event[cpu][event], value);
		    break;
		case MEMWRITE:
		    ret = perf_event_period(mem_event[cpu][event], inst_value);
		    break;
		default:
		    ret = 0;
		    break;
	    }

	    if (ret == -EINVAL)
		printk("failed to update sample period");
	}
    }
}


static int ksamplingd(void *data)
{
    unsigned long long nr_sampled = 0, nr_dram = 0, nr_nvm = 0, nr_write = 0;
    unsigned long long nr_throttled = 0, nr_lost = 0, nr_unknown = 0;
    unsigned long long nr_skip = 0;

    /* used for calculating average cpu usage of ksampled */
    struct task_struct *t = current;
    /* a unit of cputime: permil (1/1000) */
    u64 total_runtime, exec_runtime, cputime = 0;
    unsigned long total_cputime, elapsed_cputime, cur;
    /* used for periodic checks*/
    unsigned long cpucap_period = msecs_to_jiffies(1000); // 1s
    // unsigned long cpucap_period = msecs_to_jiffies(15000); // 15s
    unsigned long sample_period = 0;
    unsigned long sample_inst_period = 0;
    /* report cpu/period stat */
    unsigned long trace_cputime, trace_period = msecs_to_jiffies(1000); // 1s
    unsigned long trace_runtime;
    /* for tracking promotions/demotions */
    unsigned long prev_promoted = 0, prev_demoted = 0;
    unsigned long vm_events[NR_VM_EVENT_ITEMS];
    /* for timeout */ 
	unsigned long sleep_timeout;
	const struct cpumask *sample_mask;

    /* for analytic purpose */
    unsigned long hr_dram = 0, hr_nvm = 0;

    /* orig impl: see read_sum_exec_runtime() */
    trace_runtime = total_runtime = exec_runtime = t->se.sum_exec_runtime;

    trace_cputime = total_cputime = elapsed_cputime = jiffies;
    sleep_timeout = usecs_to_jiffies(2000);

    /* TODO implements per-CPU node ksamplingd by using pg_data_t */
    /* Currently uses a single CPU node(0) */
	sample_mask = htmm_sample_cpumask();
	if (!cpumask_empty(sample_mask))
	do_set_cpus_allowed(access_sampling, sample_mask);

    /* Initialize promotion/demotion counters */
    all_vm_events(vm_events);
    prev_promoted = vm_events[HTMM_NR_PROMOTED];
    prev_demoted = vm_events[HTMM_NR_DEMOTED];

    while (!kthread_should_stop()) {
	int cpu, event, cond = false;
    
	if (htmm_mode == HTMM_NO_MIG) {
	    msleep_interruptible(10000);
	    continue;
	}
	
	for_each_cpu(cpu, sample_mask) {
	    for (event = 0; event < N_HTMMEVENTS; event++) {
		do {
		    struct perf_buffer *rb;
		    struct perf_event_mmap_page *up;
		    struct perf_event_header ph;
		    struct htmm_event he;
		    __u64 head, tail, avail;

		    if (!mem_event || !mem_event[cpu] || !mem_event[cpu][event]) {
				break;
		    }

		    __sync_synchronize();

		    rb = mem_event[cpu][event]->rb;
		    if (!rb) {
				pr_warn_ratelimited("htmm: event->rb is NULL for cpu=%d event=%d, event_state=%d\n", 
							cpu, event, mem_event[cpu][event]->state);
				break;
		    }
		    /* perf_buffer is ring buffer */
		    up = READ_ONCE(rb->user_page);
		    head = READ_ONCE(up->data_head);
		    tail = READ_ONCE(up->data_tail);
		    if (head == tail) {
				if (cpu < 16) {
					nr_skip++;

				}
				break;
		    }

		    avail = head - tail;
		    if (avail > (BUFFER_SIZE * ksampled_max_sample_ratio / 100)) {
			cond = true;
		    } else if (avail < (BUFFER_SIZE * ksampled_min_sample_ratio / 100)) {
			cond = false;
		    }

		    if (avail < sizeof(ph))
			break;

		    /* read barrier */
		    smp_rmb();

		    if (!htmm_perf_rb_copy(rb, tail, &ph, sizeof(ph)))
			break;

		    if (ph.size < sizeof(ph) || ph.size > avail) {
			WRITE_ONCE(up->data_tail, head);
			break;
		    }

		    switch (ph.type) {
			case PERF_RECORD_SAMPLE: {
			    struct pebs_rec rec;

			    if (ph.size < sizeof(he) ||
				!htmm_perf_rb_copy(rb, tail, &he, sizeof(he)) ||
				!valid_va(he.addr)) {
				break;
			    }

			    rec.cyc = rdtsc();
			    rec.va = he.addr;
			    rec.ip = he.ip;
			    rec.cpu = cpu;
			    rec.evt = event;
				if (memtis_trace_file) {
					ssize_t written;

					written = kernel_write(memtis_trace_file,
							       &rec,
							       sizeof(rec),
							       &memtis_trace_pos);
					if (written != sizeof(rec))
						pr_warn_ratelimited("htmm: trace write failed (%zd)\n",
								    written);
				}

			    update_pginfo(he.pid, he.addr, event, rec.cyc, rec.ip);

			    //count_vm_event(HTMM_NR_SAMPLED);
			    nr_sampled++;

			    if (event == DRAMREAD) {
				nr_dram++;
				hr_dram++;
			    }
			    else if (event == CXLREAD || event == NVMREAD) {
				nr_nvm++;
				hr_nvm++;
			    }
			    else
				nr_write++;
			    break;
			}
			case PERF_RECORD_THROTTLE:
			case PERF_RECORD_UNTHROTTLE:
			    nr_throttled++;
			    break;
			case PERF_RECORD_LOST_SAMPLES:
			    nr_lost ++;
			    break;
			default:
			    nr_unknown++;
			    break;
		    }
		    if (nr_sampled % 500000 == 0) {
			trace_printk("nr_sampled: %llu, nr_dram: %llu, nr_nvm: %llu, nr_write: %llu, nr_throttled: %llu \n", nr_sampled, nr_dram, nr_nvm, nr_write,
				nr_throttled);
			nr_dram = 0;
			nr_nvm = 0;
			nr_write = 0;
		    }
		    /* read, write barrier */
		    smp_mb();
		    WRITE_ONCE(up->data_tail, tail + ph.size);
		} while (cond);
	    }
	}
	/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature */
	if (!ksampled_soft_cpu_quota)
	    continue;

	/* sleep */
	schedule_timeout_interruptible(sleep_timeout);

	/* check elasped time */
	cur = jiffies;
	if ((cur - elapsed_cputime) >= cpucap_period) {
	    u64 cur_runtime = t->se.sum_exec_runtime;
	    exec_runtime = cur_runtime - exec_runtime; //ns
	    elapsed_cputime = jiffies_to_usecs(cur - elapsed_cputime); //us
	    if (!cputime) {
		u64 cur_cputime = div64_u64(exec_runtime, elapsed_cputime);
		// EMA with the scale factor (0.2)
		cputime = ((cur_cputime << 3) + (cputime << 1)) / 10;
	    } else
		cputime = div64_u64(exec_runtime, elapsed_cputime);

	    /* to prevent frequent updates, allow for a slight variation of +/- 0.5% */
	    if (cputime > (ksampled_soft_cpu_quota + 5) &&
		    sample_period != pcount) {
		/* need to increase the sample period */
		/* only increase by 1 */
		unsigned long tmp1 = sample_period, tmp2 = sample_inst_period;
		increase_sample_period(&sample_period, &sample_inst_period);
		if (tmp1 != sample_period || tmp2 != sample_inst_period)
		    pebs_update_period(get_sample_period(sample_period),
				       get_sample_inst_period(sample_inst_period));
	    } else if (cputime < (ksampled_soft_cpu_quota - 5) && sample_period) {
		unsigned long tmp1 = sample_period, tmp2 = sample_inst_period;
		decrease_sample_period(&sample_period, &sample_inst_period);
		if (tmp1 != sample_period || tmp2 != sample_inst_period)
		    pebs_update_period(get_sample_period(sample_period),
				    get_sample_inst_period(sample_inst_period));
	    }
	    /* does it need to prevent ping-pong behavior? */
	    
	    elapsed_cputime = cur;
	    exec_runtime = cur_runtime;
	}

	/* This is used for reporting the sample period and cputime */
	if (cur - trace_cputime >= trace_period) {
	    unsigned long hr = 0;
	    unsigned long promoted_per_sec, demoted_per_sec;
	    u64 cur_runtime = t->se.sum_exec_runtime;
	    trace_runtime = cur_runtime - trace_runtime;
	    trace_cputime = jiffies_to_usecs(cur - trace_cputime);
	    trace_cputime = div64_u64(trace_runtime, trace_cputime);
	    
	    if (hr_dram + hr_nvm == 0)
		hr = 0;
	    else
		hr = hr_dram * 10000 / (hr_dram + hr_nvm);

	    all_vm_events(vm_events);
	    promoted_per_sec = vm_events[HTMM_NR_PROMOTED] - prev_promoted;
	    demoted_per_sec = vm_events[HTMM_NR_DEMOTED] - prev_demoted;
	    prev_promoted = vm_events[HTMM_NR_PROMOTED];
	    prev_demoted = vm_events[HTMM_NR_DEMOTED];

	    pr_info("sample_period: %lu || cputime: %lu || hit_ratio: %lu || promoted: %lu || demoted: %lu\n",
		    get_sample_period(sample_period), trace_cputime, hr, promoted_per_sec, demoted_per_sec);
	    
	    hr_dram = hr_nvm = 0;
	    trace_cputime = cur;
	    trace_runtime = cur_runtime;
	}
    }

    total_runtime = (t->se.sum_exec_runtime) - total_runtime; // ns
    total_cputime = jiffies_to_usecs(jiffies - total_cputime); // us

    printk("nr_sampled: %llu, nr_throttled: %llu, nr_lost: %llu\n", nr_sampled, nr_throttled, nr_lost);
    printk("total runtime: %llu ns, total cputime: %lu us, cpu usage: %llu\n",
	    total_runtime, total_cputime, (total_runtime) / total_cputime);

    return 0;
}

static int ksamplingd_run(void)
{
    int err = 0;
    
    if (!access_sampling) {
	access_sampling = kthread_run(ksamplingd, NULL, "ksamplingd");
	if (IS_ERR(access_sampling)) {
	    err = PTR_ERR(access_sampling);
	    access_sampling = NULL;
	}
    }
    return err;
}

int ksamplingd_init(pid_t pid, int node)
{
    int ret;

    if (access_sampling)
	return 0;

    ret = pebs_init(pid, node);
    if (ret) {
	printk("htmm__perf_event_init failure... ERROR:%d\n", ret);
	return 0;
    }

	pebs_enable();

    return ksamplingd_run();
}

void ksamplingd_exit(void)
{
    if (access_sampling) {
	kthread_stop(access_sampling);
	access_sampling = NULL;
    }
    pebs_disable();
}

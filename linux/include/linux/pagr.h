#include <uapi/linux/perf_event.h>

#define DEFERRED_SPLIT_ISOLATED 1

#define BUFFER_SIZE	32 /* 128: 1MB */
#define CPUS_PER_SOCKET 16
#define MAX_MIGRATION_RATE_IN_MBPS  2048 /* 2048MB per sec */

#define PAGR_HISTORY_SIZE	16
#define PAGR_ENTRY_TABLE_SIZE	1024
#define PAGR_MAX_NEIGHBORS	4
#define PAGR_PREDICTION_DEPTH	16
#define PAGR_MAX_PREDICTIONS	(PAGR_MAX_NEIGHBORS * PAGR_PREDICTION_DEPTH)
#define PAGR_MAX_PREDICTIONS_PER_SAMPLE	8
#define PAGR_QUEUE_SIZE		4096
#define PAGR_MAX_MIGRATE_BATCH	64

#define PAGR_GRAPH_MAGIC	0x3146524752474150ULL /* "PAGRGRF1" */
#define PAGR_GRAPH_VERSION	1
#define PAGR_GRAPH_INVALID_IDX	(~0U)

#define PAGR_PAGE_IN_HISTORY	0
#define PAGR_PAGE_PREDICTED	1

extern unsigned int pagr_fast_threshold_min_percent;
extern unsigned int pagr_fast_threshold_power;
extern unsigned int pagr_fast_threshold_min_samples;
extern unsigned int pagr_max_predictions_per_sample;
extern unsigned int pagr_trace_enabled;
extern unsigned int pagr_graph_enabled;
extern unsigned int pagr_graph_sample_interval;
extern unsigned int pagr_debug_interval_ms;
extern unsigned int pagr_verbose;


/* pebs events */
#define DRAM_LLC_LOAD_MISS  0x1d3
#define REMOTE_DRAM_LLC_LOAD_MISS   0x4d3
// #define REMOTE_DRAM_LLC_LOAD_MISS   0x2d3
#define NVM_LLC_LOAD_MISS   0x4d3
// #define NVM_LLC_LOAD_MISS   0x80d1
#define ALL_STORES	    0x82d0
#define ALL_LOADS	    0x81d0
#define STLB_MISS_STORES    0x12d0
#define STLB_MISS_LOADS	    0x11d0

/* tmm option */
#define HTMM_NO_MIG	    0x0	/* unused */
#define	HTMM_BASELINE	    0x1 /* unused */
#define HTMM_HUGEPAGE_OPT   0x2 /* only used */
#define HTMM_HUGEPAGE_OPT_V2	0x3 /* unused */

/**/
#define DRAM_ACCESS_LATENCY 80
#define NVM_ACCESS_LATENCY  270
#define CXL_ACCESS_LATENCY  170
#define DELTA_CYCLES	(NVM_ACCESS_LATENCY - DRAM_ACCESS_LATENCY)

#define PCOUNT 30
/* only prime numbers */
static const unsigned int pebs_period_list[PCOUNT] = {
    199,    // 200 - min
    293,    // 300
    401,    // 400
    499,    // 500
    599,    // 600
    701,    // 700
    797,    // 800
    907,    // 900
    997,    // 1000
    1201,   // 1200
    1399,   // 1400
    1601,   // 1600
    1801,   // 1800
    1999,   // 2000
    2503,   // 2500
    3001,   // 3000
    3499,   // 3500
    4001,   // 4000
    4507,   // 4507
    4999,   // 5000
    6007,   // 6000
    7001,   // 7000
    7993,   // 8000
    9001,   // 9000
    10007,  // 10000
    12007,  // 12000
    13999,  // 14000
    16001,  // 16000
    17989,  // 18000
    19997,  // 20000 - max
};

#define pinstcount 5
/* this is for store instructions */
static const unsigned int pebs_inst_period_list[pinstcount] ={
    100003, // 0.1M
    300007, // 0.3M
    600011, // 0.6M
    1000003,// 1.0M
    1500003,// 1.5M
};

struct htmm_event {
    struct perf_event_header header;
    __u64 ip;
    __u32 pid, tid;
    __u64 addr;
};

struct mem_cgroup_per_node;

enum events {
    DRAMREAD = 0,
    NVMREAD = 1,
    MEMWRITE = 2,
    TLB_MISS_LOADS = 3,
    TLB_MISS_STORES = 4,
    CXLREAD = 5, // emulated by remote DRAM node
    N_HTMMEVENTS
};

/* htmm_core.c */
extern void htmm_mm_init(struct mm_struct *mm);
extern void htmm_mm_exit(struct mm_struct *mm);
extern void __prep_transhuge_page_for_htmm(struct mm_struct *mm, struct page *page);
extern void prep_transhuge_page_for_htmm(struct vm_area_struct *vma,
					 struct page *page);
extern void clear_transhuge_pginfo(struct page *page);
extern void copy_transhuge_pginfo(struct page *page,
				  struct page *newpage);
extern pginfo_t *get_compound_pginfo(struct page *page, unsigned long address);

extern void check_transhuge_cooling(void *arg, struct page *page, bool locked);
extern void check_base_cooling(pginfo_t *pginfo, struct page *page, bool locked);
extern int set_page_coolstatus(struct page *page, pte_t *pte, struct mm_struct *mm);

extern void set_lru_adjusting(struct mem_cgroup *memcg, bool inc_thres);

extern void update_pginfo(pid_t pid, unsigned long address, enum events e, unsigned long cyc, unsigned long ip);
extern int htmm_pred_log_start(void);
extern void htmm_pred_log_stop(void);

struct pagr_graph_header {
	__u64 magic;
	__u32 version;
	__u32 record_size;
} __attribute__((packed));

struct pagr_graph_record {
	__u64 log_cyc;
	__u64 src_va;
	__u64 dst_va;
	__u64 src_pfn;
	__u64 dst_pfn;
	__u64 src_cyc;
	__u64 dst_cyc;
	__u64 src_ip;
	__u64 dst_ip;
	__u64 distance;
	__u64 time_diff;
	__u64 threshold;
	__u64 avg_dist;
	__u32 src_idx;
	__u32 dst_idx;
	__u32 slot;
	__u32 replaced_idx;
	__u8 event;
	__u8 reserved[7];
} __attribute__((packed));

enum pagr_graph_event {
	PAGR_GRAPH_EDGE_INSERT = 1,
	PAGR_GRAPH_EDGE_REFRESH = 2,
	PAGR_GRAPH_EDGE_REPLACE = 3,
};

extern void htmm_log_pagr_graph_records(struct pagr_graph_record *records,
					unsigned int nr_records);

extern bool deferred_split_huge_page_for_htmm(struct page *page);
extern unsigned long deferred_split_scan_for_htmm(struct mem_cgroup_per_node *pn,
						  struct list_head *split_list);
extern void putback_split_pages(struct list_head *split_list, struct lruvec *lruvec);

extern bool check_split_huge_page(struct mem_cgroup *memcg, struct page *meta, bool hot);
extern bool move_page_to_deferred_split_queue(struct mem_cgroup *memcg, struct page *page);

extern void move_page_to_active_lru(struct page *page);
extern void move_page_to_inactive_lru(struct page *page);


extern struct page *get_meta_page(struct page *page);
extern unsigned int get_accesses_from_idx(unsigned int idx);
extern unsigned int get_idx(unsigned long num);
extern int get_skew_idx(unsigned long num);
extern void uncharge_htmm_pte(pte_t *pte, struct mem_cgroup *memcg);
extern void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg);
extern void charge_htmm_page(struct page *page, struct mem_cgroup *memcg);

/* pagr_algo.c */
extern void pagr_add_page(struct page *page, unsigned long va,
			  unsigned long cyc, unsigned long ip);
extern int pagr_predict_pages(struct page *page,
			      struct page **out_predictions);
extern int queue_pagr_prediction(struct page *page);
extern unsigned long process_pagr_predictions(pg_data_t *pgdat);
extern bool pagr_predictions_pending(void);
extern unsigned long migrate_pagr_predicted_page(pg_data_t *pgdat,
						 struct page *page);
extern void pagr_note_access(bool fast);
extern void pagr_read_access_counters(u64 *fast, u64 *slow);
extern void pagr_reset_access_counters(void);

enum pagr_mig_dbg_event {
	PAGR_MIG_DBG_ATTEMPT,
	PAGR_MIG_DBG_ATTEMPT_ACTIVE,
	PAGR_MIG_DBG_ATTEMPT_INACTIVE,
	PAGR_MIG_DBG_ACTIVATED,
	PAGR_MIG_DBG_NON_THP,
	PAGR_MIG_DBG_STALE_NODE,
	PAGR_MIG_DBG_NO_TARGET,
	PAGR_MIG_DBG_THP_UNSUPPORTED,
	PAGR_MIG_DBG_ISOLATE_FAIL,
	PAGR_MIG_DBG_UNEVICTABLE_OR_WRITEBACK,
	PAGR_MIG_DBG_SUCCESS,
	PAGR_MIG_DBG_FAILED,
};

extern void pagr_debug_note_migration(enum pagr_mig_dbg_event event,
				      unsigned long nr_pages);
extern void pagr_debug_note_lru_migration(int promotion,
					  unsigned long nr_pages);
extern void pagr_debug_dump(const char *where);

extern void set_lru_split_pid(pid_t pid);
extern void adjust_active_threshold(pid_t pid);
extern void set_lru_cooling_pid(pid_t pid);

/* htmm_sampler.c */
extern int ksamplingd_init(pid_t pid, int node);
extern void ksamplingd_exit(void);

static inline unsigned long get_sample_period(unsigned long cur) {
    if (cur < 0)
	return 0;
    else if (cur < PCOUNT)
	return pebs_period_list[cur];
    else
	return pebs_period_list[PCOUNT - 1];
}

static inline unsigned long get_sample_inst_period(unsigned long cur) {
    if (cur < 0)
	return 0;
    else if (cur < pinstcount)
	return pebs_inst_period_list[cur];
    else
	return pebs_inst_period_list[pinstcount - 1];
}
#if 1
static inline void increase_sample_period(unsigned long *llc_period,
					  unsigned long *inst_period) {
    unsigned long p;
    p = *llc_period;
    if (++p < PCOUNT)
	*llc_period = p;
    
    p = *inst_period;
    if (++p < pinstcount)
	*inst_period = p;
}

static inline void decrease_sample_period(unsigned long *llc_period,
					  unsigned long *inst_period) {
    unsigned long p;
    p = *llc_period;
    if (p > 0)
	*llc_period = p - 1;
    
    p = *inst_period;
    if (p > 0)
	*inst_period = p - 1;
}
#else
static inline unsigned int increase_sample_period(unsigned int cur,
						  unsigned int next) {
    do {
	cur++;
    } while (pebs_period_list[cur] < next && cur < PCOUNT);
    
    return cur < PCOUNT ? cur : PCOUNT - 1;
}

static inline unsigned int decrease_sample_period(unsigned int cur,
						  unsigned int next) {
    do {
	cur--;
    } while (pebs_period_list[cur] > next && cur > 0);
    
    return cur;
}
#endif


/* htmm_migrater.c */
#define HTMM_MIN_FREE_PAGES 256 * 10 // 10MB
extern unsigned long get_nr_lru_pages_node(struct mem_cgroup *memcg, pg_data_t *pgdat);
extern void add_memcg_to_kmigraterd(struct mem_cgroup *memcg, int nid);
extern void del_memcg_from_kmigraterd(struct mem_cgroup *memcg, int nid);
extern unsigned long get_memcg_demotion_watermark(unsigned long max_nr_pages);
extern unsigned long get_memcg_promotion_watermark(unsigned long max_nr_pages);
extern void kmigraterd_wakeup(int nid);
extern int kmigraterd_init(void);
extern void kmigraterd_stop(void);

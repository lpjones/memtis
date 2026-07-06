#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/math64.h>
#include <linux/node.h>
#include <linux/pagr.h>
#include <linux/spinlock.h>
#include <asm/msr.h>

#include "internal.h"

#define PAGR_SCALE 1000000ULL
#define PAGR_DEC_FAST_NUM 10000ULL
#define PAGR_DEC_SLOW_NUM 200ULL
#define PAGR_DEC_DIST_NUM 100ULL
#define PAGR_DEC_MIG_NUM 10000ULL
#define PAGR_NEIGHBOR_DEC_NUM 1100ULL
#define PAGR_NEIGHBOR_DEC_DEN 1000ULL
#define PAGR_INVALID_ENTRY (~0U)

#define PAGR_ABS_DIFF(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))
#define PAGR_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PAGR_MAX(a, b) ((a) > (b) ? (a) : (b))
#define PAGR_CLIP(x, a, b) (PAGR_MIN(PAGR_MAX((x), (a)), (b)))

struct pagr_neighbor {
	unsigned int idx;
	u64 distance;
	u64 time_diff;
};

struct pagr_entry {
	struct page *page;
	unsigned long va;
	unsigned long cyc;
	unsigned long ip;
	unsigned long stamp;
	bool predicted;
	struct pagr_neighbor neighbors[PAGR_MAX_NEIGHBORS];
};

struct pagr_queue_entry {
	unsigned int idx;
	unsigned long stamp;
	int src_nid;
	int target_nid;
	u64 enqueue_cyc;
};

static struct pagr_entry pagr_entries[PAGR_ENTRY_TABLE_SIZE];
static unsigned int pagr_history[PAGR_HISTORY_SIZE];
static unsigned int pagr_history_count;
static DEFINE_SPINLOCK(pagr_lock);
static unsigned long pagr_next_stamp;

static u64 top_va = 2 * PAGR_SCALE, bot_va = 1 * PAGR_SCALE;
static u64 top_cyc = 2 * PAGR_SCALE, bot_cyc = 1 * PAGR_SCALE;
static u64 top_ip = 2 * PAGR_SCALE, bot_ip = 1 * PAGR_SCALE;
static u64 pagr_avg_dist = 1 * PAGR_SCALE;
static u64 pagr_bot_dist = 1 * PAGR_SCALE;
static u64 pagr_mig_queue_time;
static u64 pagr_mig_move_time;

static struct pagr_queue_entry pagr_prediction_queue[PAGR_QUEUE_SIZE];
static unsigned int pagr_queue_head;
static unsigned int pagr_queue_tail;
static DEFINE_SPINLOCK(pagr_queue_lock);

enum pagr_stat_idx {
	PAGR_STAT_ADD_CALLS,
	PAGR_STAT_ADD_NON_THP,
	PAGR_STAT_ADD_ALLOC_FAIL,
	PAGR_STAT_ENTRY_NEW,
	PAGR_STAT_ENTRY_EXISTING,
	PAGR_STAT_ENTRY_EVICTED,
	PAGR_STAT_HISTORY_RECORDED,
	PAGR_STAT_HISTORY_WRAPPED,
	PAGR_STAT_NEIGHBOR_UPDATES,
	PAGR_STAT_NEIGHBOR_CANDIDATES,
	PAGR_STAT_PRED_CALLS,
	PAGR_STAT_PRED_NO_ENTRY,
	PAGR_STAT_PRED_SKIP_EMPTY,
	PAGR_STAT_PRED_SKIP_DISTANCE,
	PAGR_STAT_PRED_SKIP_STALE,
	PAGR_STAT_PRED_SKIP_TIME,
	PAGR_STAT_PRED_SKIP_DUP_SELECTED,
	PAGR_STAT_PRED_SKIP_ALREADY_PRED,
	PAGR_STAT_PRED_SKIP_NOT_PROMOTABLE,
	PAGR_STAT_PRED_SELECTED,
	PAGR_STAT_PRED_ZERO,
	PAGR_STAT_QUEUE_ATTEMPTS,
	PAGR_STAT_QUEUE_NON_THP,
	PAGR_STAT_QUEUE_NOT_PROMOTABLE,
	PAGR_STAT_QUEUE_MISSING_HISTORY,
	PAGR_STAT_QUEUE_DUPLICATE,
	PAGR_STAT_QUEUE_FULL,
	PAGR_STAT_QUEUE_ACCEPTED,
	PAGR_STAT_DEQUEUE_EMPTY,
	PAGR_STAT_DEQUEUE_REQUEUED,
	PAGR_STAT_PROCESS_CALLS,
	PAGR_STAT_PROCESS_STALE,
	PAGR_STAT_PROCESS_MIGRATE_ENTRIES,
	PAGR_STAT_PROCESS_MIGRATE_BASE_PAGES,
	PAGR_STAT_PROCESS_MIGRATE_FAILED,
	PAGR_STAT_MIG_ATTEMPT,
	PAGR_STAT_MIG_ATTEMPT_ACTIVE,
	PAGR_STAT_MIG_ATTEMPT_INACTIVE,
	PAGR_STAT_MIG_ACTIVATED,
	PAGR_STAT_MIG_NON_THP,
	PAGR_STAT_MIG_STALE_NODE,
	PAGR_STAT_MIG_NO_TARGET,
	PAGR_STAT_MIG_THP_UNSUPPORTED,
	PAGR_STAT_MIG_ISOLATE_FAIL,
	PAGR_STAT_MIG_UNEVICTABLE_OR_WRITEBACK,
	PAGR_STAT_MIG_SUCCESS_ENTRIES,
	PAGR_STAT_MIG_SUCCESS_BASE_PAGES,
	PAGR_STAT_MIG_FAILED,
	PAGR_STAT_LRU_PROMOTE_BASE_PAGES,
	PAGR_STAT_LRU_DEMOTE_BASE_PAGES,
	PAGR_STAT_NR,
};

static atomic64_t pagr_stats[PAGR_STAT_NR];
static DEFINE_SPINLOCK(pagr_debug_lock);
static unsigned long pagr_next_debug_jiffies;

static inline void pagr_stat_inc(enum pagr_stat_idx idx)
{
	atomic64_inc(&pagr_stats[idx]);
}

static inline void pagr_stat_add(enum pagr_stat_idx idx, unsigned long val)
{
	if (val)
		atomic64_add(val, &pagr_stats[idx]);
}

static inline s64 pagr_stat_read(enum pagr_stat_idx idx)
{
	return atomic64_read(&pagr_stats[idx]);
}

static unsigned int queue_count_snapshot(void)
{
	unsigned int head = READ_ONCE(pagr_queue_head);
	unsigned int tail = READ_ONCE(pagr_queue_tail);

	if (head >= tail)
		return head - tail;

	return PAGR_QUEUE_SIZE - tail + head;
}

void pagr_debug_dump(const char *where)
{
	unsigned int active_entries = 0, predicted_entries = 0;
	unsigned int history_count = READ_ONCE(pagr_history_count);
	unsigned int queue_count = queue_count_snapshot();
	unsigned long flags;
	bool have_entry_snapshot = false;
	int i;

	if (spin_trylock_irqsave(&pagr_lock, flags)) {
		for (i = 0; i < PAGR_ENTRY_TABLE_SIZE; i++) {
			if (!pagr_entries[i].page)
				continue;
			active_entries++;
			if (pagr_entries[i].predicted)
				predicted_entries++;
		}
		have_entry_snapshot = true;
		spin_unlock_irqrestore(&pagr_lock, flags);
	}

	pr_info("PAGR_DBG[%s] state entries=%u predicted_entries=%u entry_snapshot=%u history=%u queue=%u threshold=%llu avg_dist=%llu va_top=%llu va_bot=%llu cyc_top=%llu cyc_bot=%llu ip_top=%llu ip_bot=%llu mig_queue=%llu mig_move=%llu\n",
		where, active_entries, predicted_entries,
		have_entry_snapshot ? 1U : 0U, history_count, queue_count,
		READ_ONCE(pagr_bot_dist),
		READ_ONCE(pagr_avg_dist), READ_ONCE(top_va), READ_ONCE(bot_va),
		READ_ONCE(top_cyc), READ_ONCE(bot_cyc), READ_ONCE(top_ip),
		READ_ONCE(bot_ip), READ_ONCE(pagr_mig_queue_time),
		READ_ONCE(pagr_mig_move_time));

	pr_info("PAGR_DBG[%s] add add=%lld non_thp=%lld alloc_fail=%lld new=%lld existing=%lld evicted=%lld hist=%lld hist_wrap=%lld neighbor_updates=%lld neighbor_candidates=%lld\n",
		where, pagr_stat_read(PAGR_STAT_ADD_CALLS),
		pagr_stat_read(PAGR_STAT_ADD_NON_THP),
		pagr_stat_read(PAGR_STAT_ADD_ALLOC_FAIL),
		pagr_stat_read(PAGR_STAT_ENTRY_NEW),
		pagr_stat_read(PAGR_STAT_ENTRY_EXISTING),
		pagr_stat_read(PAGR_STAT_ENTRY_EVICTED),
		pagr_stat_read(PAGR_STAT_HISTORY_RECORDED),
		pagr_stat_read(PAGR_STAT_HISTORY_WRAPPED),
		pagr_stat_read(PAGR_STAT_NEIGHBOR_UPDATES),
		pagr_stat_read(PAGR_STAT_NEIGHBOR_CANDIDATES));

	pr_info("PAGR_DBG[%s] pred calls=%lld no_entry=%lld selected=%lld zero=%lld skip_empty=%lld skip_dist=%lld skip_stale=%lld skip_time=%lld skip_dup=%lld skip_already_pred=%lld skip_not_promotable=%lld\n",
		where, pagr_stat_read(PAGR_STAT_PRED_CALLS),
		pagr_stat_read(PAGR_STAT_PRED_NO_ENTRY),
		pagr_stat_read(PAGR_STAT_PRED_SELECTED),
		pagr_stat_read(PAGR_STAT_PRED_ZERO),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_EMPTY),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_DISTANCE),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_STALE),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_TIME),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_DUP_SELECTED),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_ALREADY_PRED),
		pagr_stat_read(PAGR_STAT_PRED_SKIP_NOT_PROMOTABLE));

	pr_info("PAGR_DBG[%s] queue attempts=%lld accepted=%lld drops_non_thp=%lld drops_not_promotable=%lld drops_missing_history=%lld drops_duplicate=%lld drops_full=%lld dequeue_empty=%lld dequeue_requeued=%lld process_calls=%lld process_stale=%lld process_mig_entries=%lld process_mig_base=%lld process_mig_failed=%lld\n",
		where, pagr_stat_read(PAGR_STAT_QUEUE_ATTEMPTS),
		pagr_stat_read(PAGR_STAT_QUEUE_ACCEPTED),
		pagr_stat_read(PAGR_STAT_QUEUE_NON_THP),
		pagr_stat_read(PAGR_STAT_QUEUE_NOT_PROMOTABLE),
		pagr_stat_read(PAGR_STAT_QUEUE_MISSING_HISTORY),
		pagr_stat_read(PAGR_STAT_QUEUE_DUPLICATE),
		pagr_stat_read(PAGR_STAT_QUEUE_FULL),
		pagr_stat_read(PAGR_STAT_DEQUEUE_EMPTY),
		pagr_stat_read(PAGR_STAT_DEQUEUE_REQUEUED),
		pagr_stat_read(PAGR_STAT_PROCESS_CALLS),
		pagr_stat_read(PAGR_STAT_PROCESS_STALE),
		pagr_stat_read(PAGR_STAT_PROCESS_MIGRATE_ENTRIES),
		pagr_stat_read(PAGR_STAT_PROCESS_MIGRATE_BASE_PAGES),
		pagr_stat_read(PAGR_STAT_PROCESS_MIGRATE_FAILED));

	pr_info("PAGR_DBG[%s] migrate attempts=%lld active=%lld inactive=%lld activated=%lld non_thp=%lld stale_node=%lld no_target=%lld thp_unsupported=%lld isolate_fail=%lld unevictable_writeback=%lld success_entries=%lld success_base=%lld failed=%lld lru_promote_base=%lld lru_demote_base=%lld\n",
		where, pagr_stat_read(PAGR_STAT_MIG_ATTEMPT),
		pagr_stat_read(PAGR_STAT_MIG_ATTEMPT_ACTIVE),
		pagr_stat_read(PAGR_STAT_MIG_ATTEMPT_INACTIVE),
		pagr_stat_read(PAGR_STAT_MIG_ACTIVATED),
		pagr_stat_read(PAGR_STAT_MIG_NON_THP),
		pagr_stat_read(PAGR_STAT_MIG_STALE_NODE),
		pagr_stat_read(PAGR_STAT_MIG_NO_TARGET),
		pagr_stat_read(PAGR_STAT_MIG_THP_UNSUPPORTED),
		pagr_stat_read(PAGR_STAT_MIG_ISOLATE_FAIL),
		pagr_stat_read(PAGR_STAT_MIG_UNEVICTABLE_OR_WRITEBACK),
		pagr_stat_read(PAGR_STAT_MIG_SUCCESS_ENTRIES),
		pagr_stat_read(PAGR_STAT_MIG_SUCCESS_BASE_PAGES),
		pagr_stat_read(PAGR_STAT_MIG_FAILED),
		pagr_stat_read(PAGR_STAT_LRU_PROMOTE_BASE_PAGES),
		pagr_stat_read(PAGR_STAT_LRU_DEMOTE_BASE_PAGES));
}

static void pagr_debug_maybe_dump(const char *where)
{
	unsigned long flags;
	unsigned long now = jiffies;

	if (time_before(now, READ_ONCE(pagr_next_debug_jiffies)))
		return;

	if (!spin_trylock_irqsave(&pagr_debug_lock, flags))
		return;

	if (time_after_eq(now, READ_ONCE(pagr_next_debug_jiffies))) {
		WRITE_ONCE(pagr_next_debug_jiffies, now + HZ);
		pagr_debug_dump(where);
	}

	spin_unlock_irqrestore(&pagr_debug_lock, flags);
}

void pagr_debug_note_migration(enum pagr_mig_dbg_event event,
			       unsigned long nr_pages)
{
	switch (event) {
	case PAGR_MIG_DBG_ATTEMPT:
		pagr_stat_inc(PAGR_STAT_MIG_ATTEMPT);
		break;
	case PAGR_MIG_DBG_ATTEMPT_ACTIVE:
		pagr_stat_inc(PAGR_STAT_MIG_ATTEMPT_ACTIVE);
		break;
	case PAGR_MIG_DBG_ATTEMPT_INACTIVE:
		pagr_stat_inc(PAGR_STAT_MIG_ATTEMPT_INACTIVE);
		break;
	case PAGR_MIG_DBG_ACTIVATED:
		pagr_stat_inc(PAGR_STAT_MIG_ACTIVATED);
		break;
	case PAGR_MIG_DBG_NON_THP:
		pagr_stat_inc(PAGR_STAT_MIG_NON_THP);
		break;
	case PAGR_MIG_DBG_STALE_NODE:
		pagr_stat_inc(PAGR_STAT_MIG_STALE_NODE);
		break;
	case PAGR_MIG_DBG_NO_TARGET:
		pagr_stat_inc(PAGR_STAT_MIG_NO_TARGET);
		break;
	case PAGR_MIG_DBG_THP_UNSUPPORTED:
		pagr_stat_inc(PAGR_STAT_MIG_THP_UNSUPPORTED);
		break;
	case PAGR_MIG_DBG_ISOLATE_FAIL:
		pagr_stat_inc(PAGR_STAT_MIG_ISOLATE_FAIL);
		break;
	case PAGR_MIG_DBG_UNEVICTABLE_OR_WRITEBACK:
		pagr_stat_inc(PAGR_STAT_MIG_UNEVICTABLE_OR_WRITEBACK);
		break;
	case PAGR_MIG_DBG_SUCCESS:
		pagr_stat_inc(PAGR_STAT_MIG_SUCCESS_ENTRIES);
		pagr_stat_add(PAGR_STAT_MIG_SUCCESS_BASE_PAGES, nr_pages);
		break;
	case PAGR_MIG_DBG_FAILED:
		pagr_stat_inc(PAGR_STAT_MIG_FAILED);
		break;
	}

	pagr_debug_maybe_dump("migration");
}

void pagr_debug_note_lru_migration(int promotion, unsigned long nr_pages)
{
	if (promotion)
		pagr_stat_add(PAGR_STAT_LRU_PROMOTE_BASE_PAGES, nr_pages);
	else
		pagr_stat_add(PAGR_STAT_LRU_DEMOTE_BASE_PAGES, nr_pages);

	pagr_debug_maybe_dump(promotion ? "lru_promote" : "lru_demote");
}

static void clear_entry_neighbors(struct pagr_entry *entry)
{
	int i;

	for (i = 0; i < PAGR_MAX_NEIGHBORS; i++) {
		entry->neighbors[i].idx = PAGR_INVALID_ENTRY;
		entry->neighbors[i].distance = 0;
		entry->neighbors[i].time_diff = 0;
	}
}

static void clear_neighbor_refs(unsigned int old_idx)
{
	int i, j;

	for (i = 0; i < PAGR_ENTRY_TABLE_SIZE; i++) {
		struct pagr_entry *entry = &pagr_entries[i];

		if (!entry->page)
			continue;

		for (j = 0; j < PAGR_MAX_NEIGHBORS; j++) {
			if (entry->neighbors[j].idx == old_idx) {
				entry->neighbors[j].idx = PAGR_INVALID_ENTRY;
				entry->neighbors[j].distance = 0;
				entry->neighbors[j].time_diff = 0;
			}
		}
	}
}

static void clear_history_refs(unsigned int old_idx)
{
	int i;

	for (i = 0; i < pagr_history_count; i++) {
		if (pagr_history[i] != old_idx)
			continue;

		pagr_history[i] = pagr_history[pagr_history_count - 1];
		pagr_history_count--;
		i--;
	}
}

static int find_entry(struct page *page)
{
	int i;

	page = compound_head(page);

	for (i = 0; i < PAGR_ENTRY_TABLE_SIZE; i++) {
		if (pagr_entries[i].page == page)
			return i;
	}

	return -1;
}

static unsigned long new_entry_stamp(void)
{
	if (++pagr_next_stamp == 0)
		pagr_next_stamp = 1;

	return pagr_next_stamp;
}

static inline u64 update_top(u64 top, u64 val)
{
	if (val < top)
		return (PAGR_DEC_SLOW_NUM * val +
			(PAGR_SCALE - PAGR_DEC_SLOW_NUM) * top) / PAGR_SCALE;

	return (PAGR_DEC_FAST_NUM * val +
		(PAGR_SCALE - PAGR_DEC_FAST_NUM) * top) / PAGR_SCALE;
}

static inline u64 update_bot(u64 bot, u64 val)
{
	if (val < bot)
		return (PAGR_DEC_FAST_NUM * val +
			(PAGR_SCALE - PAGR_DEC_FAST_NUM) * bot) / PAGR_SCALE;

	return (PAGR_DEC_SLOW_NUM * val +
		(PAGR_SCALE - PAGR_DEC_SLOW_NUM) * bot) / PAGR_SCALE;
}

static inline u64 update_mig_time(u64 avg, u64 val)
{
	if (!avg)
		return val;

	return (PAGR_DEC_MIG_NUM * val +
		(PAGR_SCALE - PAGR_DEC_MIG_NUM) * avg) / PAGR_SCALE;
}

static u64 scaled_clipped_diff(u64 diff, u64 low, u64 high)
{
	if (diff < DIV_ROUND_UP_ULL(low, PAGR_SCALE))
		return low;
	if (diff > div64_u64(high, PAGR_SCALE))
		return high;
	return diff * PAGR_SCALE;
}

static u64 scaled_diff(u64 diff)
{
	if (diff > div64_u64(U64_MAX, PAGR_SCALE))
		return U64_MAX;
	return diff * PAGR_SCALE;
}

static u64 normalize_diff(u64 diff, u64 *top, u64 *bot)
{
	u64 clipped = scaled_clipped_diff(diff, *bot / 10, *top * 10);
	u64 scaled = scaled_diff(diff);
	u64 delta;

	*top = update_top(*top, clipped);
	*bot = update_bot(*bot, clipped);

	if (*top <= *bot)
		return 0;

	delta = PAGR_ABS_DIFF(scaled, *bot);
	return mul_u64_u64_div_u64(delta, PAGR_SCALE, *top - *bot);
}

static u64 calc_distance(struct pagr_entry *a, struct pagr_entry *b)
{
	u64 va_diff, cyc_diff, ip_diff, distance, dist_clip;

	va_diff = normalize_diff(PAGR_ABS_DIFF(a->va, b->va),
				 &top_va, &bot_va);
	cyc_diff = normalize_diff(PAGR_ABS_DIFF(a->cyc, b->cyc),
				  &top_cyc, &bot_cyc);
	ip_diff = normalize_diff(PAGR_ABS_DIFF(a->ip, b->ip),
				 &top_ip, &bot_ip);

	distance = va_diff + cyc_diff + ip_diff;
	dist_clip = PAGR_CLIP(distance, pagr_bot_dist / 10, pagr_avg_dist * 10);
	pagr_bot_dist = update_bot(pagr_bot_dist, dist_clip);
	pagr_avg_dist = (PAGR_DEC_DIST_NUM * dist_clip +
			 (PAGR_SCALE - PAGR_DEC_DIST_NUM) * pagr_avg_dist) /
			PAGR_SCALE;

	return distance ? distance : 1;
}

static void reset_entry(unsigned int idx)
{
	struct pagr_entry *entry = &pagr_entries[idx];

	clear_neighbor_refs(idx);
	clear_history_refs(idx);
	clear_entry_neighbors(entry);
	entry->page = NULL;
	entry->va = 0;
	entry->cyc = 0;
	entry->ip = 0;
	entry->stamp = 0;
	entry->predicted = false;
}

static int alloc_entry(void)
{
	u64 min_cyc = U64_MAX;
	u64 min_unpredicted_cyc = U64_MAX;
	int fallback = -1;
	int victim = -1;
	int i;

	for (i = 0; i < PAGR_ENTRY_TABLE_SIZE; i++) {
		struct pagr_entry *entry = &pagr_entries[i];

		if (!entry->page)
			return i;

		if (entry->cyc < min_cyc) {
			min_cyc = entry->cyc;
			fallback = i;
		}

		if (!entry->predicted && entry->cyc < min_unpredicted_cyc) {
			min_unpredicted_cyc = entry->cyc;
			victim = i;
		}
	}

	if (victim < 0)
		victim = fallback;
	if (victim >= 0) {
		pagr_stat_inc(PAGR_STAT_ENTRY_EVICTED);
		reset_entry(victim);
	}

	return victim;
}

static bool pagr_prediction_target(struct page *page, int *src_nid,
				   int *target_nid)
{
	int src, target;

	src = page_to_nid(page);
	if (htmm_cxl_mode) {
		if (src == 0)
			return false;
		target = 0;
	} else {
		if (node_is_toptier(src))
			return false;
		target = next_promotion_node(src);
	}

	if (target == NUMA_NO_NODE || target == src)
		return false;

	if (src_nid)
		*src_nid = src;
	if (target_nid)
		*target_nid = target;
	return true;
}

static void update_neighbors(unsigned int old_idx)
{
	struct pagr_entry *old_entry = &pagr_entries[old_idx];
	int i, j;

	if (!old_entry->page)
		return;

	pagr_stat_inc(PAGR_STAT_NEIGHBOR_UPDATES);

	for (i = 0; i < PAGR_MAX_NEIGHBORS; i++) {
		if (old_entry->neighbors[i].idx != PAGR_INVALID_ENTRY)
			old_entry->neighbors[i].distance =
				old_entry->neighbors[i].distance *
				PAGR_NEIGHBOR_DEC_NUM / PAGR_NEIGHBOR_DEC_DEN;
	}

	for (i = 0; i < pagr_history_count; i++) {
		unsigned int cur_idx = pagr_history[i];
		struct pagr_entry *cur_entry;
		u64 distance;
		int candidate = -1;

		if (cur_idx == old_idx || cur_idx >= PAGR_ENTRY_TABLE_SIZE)
			continue;

		cur_entry = &pagr_entries[cur_idx];
		if (!cur_entry->page || cur_entry->cyc <= old_entry->cyc)
			continue;

		pagr_stat_inc(PAGR_STAT_NEIGHBOR_CANDIDATES);
		distance = calc_distance(old_entry, cur_entry);

		for (j = 0; j < PAGR_MAX_NEIGHBORS; j++) {
			struct pagr_neighbor *neighbor = &old_entry->neighbors[j];

			if (neighbor->idx == cur_idx) {
				candidate = j;
				neighbor->distance = 0;
				break;
			}

			if (neighbor->idx == PAGR_INVALID_ENTRY) {
				candidate = j;
				break;
			}

			if (candidate < 0 ||
			    neighbor->distance > old_entry->neighbors[candidate].distance)
				candidate = j;
		}

		if (candidate >= 0) {
			struct pagr_neighbor *neighbor =
				&old_entry->neighbors[candidate];

			if (neighbor->idx == PAGR_INVALID_ENTRY ||
			    neighbor->idx == cur_idx ||
			    distance < neighbor->distance) {
				neighbor->idx = cur_idx;
				neighbor->distance = distance;
				neighbor->time_diff = cur_entry->cyc - old_entry->cyc;
			}
		}
	}
}

static void record_history_sample(unsigned int idx)
{
	u64 min_cyc = U64_MAX;
	int old_slot = -1;
	int i;

	if (pagr_history_count < PAGR_HISTORY_SIZE) {
		pagr_history[pagr_history_count++] = idx;
		pagr_stat_inc(PAGR_STAT_HISTORY_RECORDED);
		return;
	}

	for (i = 0; i < PAGR_HISTORY_SIZE; i++) {
		unsigned int old_idx = pagr_history[i];

		if (old_idx >= PAGR_ENTRY_TABLE_SIZE ||
		    !pagr_entries[old_idx].page) {
			old_slot = i;
			break;
		}

		if (pagr_entries[old_idx].cyc < min_cyc) {
			min_cyc = pagr_entries[old_idx].cyc;
			old_slot = i;
		}
	}

	if (old_slot < 0)
		return;

	if (pagr_history[old_slot] < PAGR_ENTRY_TABLE_SIZE)
		update_neighbors(pagr_history[old_slot]);
	pagr_history[old_slot] = idx;
	pagr_stat_inc(PAGR_STAT_HISTORY_RECORDED);
	pagr_stat_inc(PAGR_STAT_HISTORY_WRAPPED);
}

void pagr_add_page(struct page *page, unsigned long va, unsigned long cyc,
		   unsigned long ip)
{
	struct pagr_entry *entry;
	unsigned long flags;
	bool predicted;
	int idx;

	pagr_stat_inc(PAGR_STAT_ADD_CALLS);

	if (!PageTransHuge(page)) {
		pagr_stat_inc(PAGR_STAT_ADD_NON_THP);
		pagr_debug_maybe_dump("add_non_thp");
		return;
	}

	page = compound_head(page);

	spin_lock_irqsave(&pagr_lock, flags);

	idx = find_entry(page);
	if (idx < 0)
		idx = alloc_entry();
	if (idx < 0) {
		spin_unlock_irqrestore(&pagr_lock, flags);
		pagr_stat_inc(PAGR_STAT_ADD_ALLOC_FAIL);
		pagr_debug_maybe_dump("add_alloc_fail");
		return;
	}

	entry = &pagr_entries[idx];
	if (!entry->page) {
		clear_entry_neighbors(entry);
		entry->stamp = new_entry_stamp();
		entry->predicted = false;
		pagr_stat_inc(PAGR_STAT_ENTRY_NEW);
	} else {
		pagr_stat_inc(PAGR_STAT_ENTRY_EXISTING);
	}

	entry->page = page;
	entry->va = va;
	entry->cyc = cyc;
	entry->ip = ip;
	predicted = entry->predicted;
	record_history_sample(idx);

	page[3].last_va = va;
	page[3].last_cyc = cyc;
	page[3].last_ip = ip;
	__set_bit(PAGR_PAGE_IN_HISTORY, &page[3].pagr_flags);
	if (predicted)
		__set_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
	else
		__clear_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);

	spin_unlock_irqrestore(&pagr_lock, flags);
	pagr_debug_maybe_dump("add_page");
}

static bool prediction_already_selected(unsigned int *selected, int count,
					unsigned int idx)
{
	int i;

	for (i = 0; i < count; i++) {
		if (selected[i] == idx)
			return true;
	}

	return false;
}

int pagr_predict_pages(struct page *page, struct page **out_predictions)
{
	unsigned int selected[PAGR_MAX_PREDICTIONS];
	unsigned long flags;
	u64 threshold, mig_time, total_time_diff = 0;
	int count = 0;
	int cur_idx;
	int depth, i;

	pagr_stat_inc(PAGR_STAT_PRED_CALLS);

	if (!PageTransHuge(page)) {
		pagr_stat_inc(PAGR_STAT_PRED_SKIP_EMPTY);
		pagr_debug_maybe_dump("predict_non_thp");
		return 0;
	}

	page = compound_head(page);

	spin_lock_irqsave(&pagr_lock, flags);

	cur_idx = find_entry(page);
	if (cur_idx < 0) {
		spin_unlock_irqrestore(&pagr_lock, flags);
		pagr_stat_inc(PAGR_STAT_PRED_NO_ENTRY);
		pagr_debug_maybe_dump("predict_no_entry");
		return 0;
	}

	threshold = pagr_bot_dist;
	if (!queue_count_snapshot() && READ_ONCE(pagr_mig_queue_time))
		WRITE_ONCE(pagr_mig_queue_time, 0);
	mig_time = READ_ONCE(pagr_mig_queue_time) +
		   READ_ONCE(pagr_mig_move_time);

	for (depth = 0; depth < PAGR_PREDICTION_DEPTH; depth++) {
		struct pagr_entry *cur_entry = &pagr_entries[cur_idx];
		struct pagr_neighbor *closest = NULL;

		for (i = 0; i < PAGR_MAX_NEIGHBORS; i++) {
			struct pagr_neighbor *neighbor = &cur_entry->neighbors[i];
			struct pagr_entry *pred_entry;
			struct page *pred_page;
			unsigned int pred_idx = neighbor->idx;

			if (pred_idx == PAGR_INVALID_ENTRY ||
			    pred_idx >= PAGR_ENTRY_TABLE_SIZE ||
			    !neighbor->distance ||
			    !neighbor->time_diff) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_EMPTY);
				continue;
			}

			if (neighbor->distance >= threshold) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_DISTANCE);
				continue;
			}

			pred_entry = &pagr_entries[pred_idx];
			if (!pred_entry->page) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_STALE);
				continue;
			}

			if (!closest || neighbor->distance < closest->distance)
				closest = neighbor;

			if (neighbor->time_diff + total_time_diff <= mig_time) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_TIME);
				continue;
			}
			if (prediction_already_selected(selected, count, pred_idx)) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_DUP_SELECTED);
				continue;
			}
			if (pred_entry->predicted) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_ALREADY_PRED);
				continue;
			}

			pred_page = pred_entry->page;
			if (!pagr_prediction_target(pred_page, NULL, NULL)) {
				pagr_stat_inc(PAGR_STAT_PRED_SKIP_NOT_PROMOTABLE);
				continue;
			}

			printk_ratelimited("PAGR: predicted page pfn=%lx va=%lx src_pfn=%lx src_va=%lx distance=%llu threshold=%llu depth=%d time_diff=%llu mig_time=%llu\n",
					   page_to_pfn(pred_page),
					   pred_entry->va,
					   page_to_pfn(cur_entry->page),
					   cur_entry->va,
					   neighbor->distance,
					   threshold,
					   depth,
					   neighbor->time_diff,
					   mig_time);
			selected[count] = pred_idx;
			out_predictions[count++] = pred_page;
			pagr_stat_inc(PAGR_STAT_PRED_SELECTED);
			if (count >= PAGR_MAX_PREDICTIONS)
				break;
		}

		if (!closest || count >= PAGR_MAX_PREDICTIONS)
			break;

		total_time_diff += closest->time_diff;
		cur_idx = closest->idx;
	}

	spin_unlock_irqrestore(&pagr_lock, flags);
	if (!count)
		pagr_stat_inc(PAGR_STAT_PRED_ZERO);
	pagr_debug_maybe_dump("predict");
	return count;
}

static unsigned int queue_count_locked(void)
{
	if (pagr_queue_head >= pagr_queue_tail)
		return pagr_queue_head - pagr_queue_tail;

	return PAGR_QUEUE_SIZE - pagr_queue_tail + pagr_queue_head;
}

int queue_pagr_prediction(struct page *page)
{
	struct pagr_queue_entry queue_entry;
	unsigned long flags;
	unsigned int next;
	unsigned long va = 0;
	unsigned long pfn = 0;
	int src_nid = NUMA_NO_NODE;
	int target_nid = NUMA_NO_NODE;
	int idx;

	pagr_stat_inc(PAGR_STAT_QUEUE_ATTEMPTS);

	if (!PageTransHuge(page)) {
		pagr_stat_inc(PAGR_STAT_QUEUE_NON_THP);
		printk_ratelimited("PAGR: drop predicted non-THP page pfn=%lx\n",
				   page_to_pfn(compound_head(page)));
		pagr_debug_maybe_dump("queue_non_thp");
		return NUMA_NO_NODE;
	}

	page = compound_head(page);
	pfn = page_to_pfn(page);

	if (!pagr_prediction_target(page, &src_nid, &target_nid)) {
		pagr_stat_inc(PAGR_STAT_QUEUE_NOT_PROMOTABLE);
		printk_ratelimited("PAGR: drop prediction not promotable pfn=%lx nid=%d\n",
				   pfn, page_to_nid(page));
		pagr_debug_maybe_dump("queue_not_promotable");
		return NUMA_NO_NODE;
	}

	spin_lock_irqsave(&pagr_lock, flags);
	idx = find_entry(page);
	if (idx < 0) {
		spin_unlock_irqrestore(&pagr_lock, flags);
		pagr_stat_inc(PAGR_STAT_QUEUE_MISSING_HISTORY);
		printk_ratelimited("PAGR: drop prediction missing history pfn=%lx\n",
				   pfn);
		pagr_debug_maybe_dump("queue_missing_history");
		return NUMA_NO_NODE;
	}

	if (pagr_entries[idx].predicted) {
		va = pagr_entries[idx].va;
		spin_unlock_irqrestore(&pagr_lock, flags);
		pagr_stat_inc(PAGR_STAT_QUEUE_DUPLICATE);
		printk_ratelimited("PAGR: drop duplicate prediction pfn=%lx va=%lx nid=%d\n",
				   pfn, va, src_nid);
		pagr_debug_maybe_dump("queue_duplicate");
		return NUMA_NO_NODE;
	}

	pagr_entries[idx].predicted = true;
	__set_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
	queue_entry.idx = idx;
	queue_entry.stamp = pagr_entries[idx].stamp;
	queue_entry.src_nid = src_nid;
	queue_entry.target_nid = target_nid;
	queue_entry.enqueue_cyc = rdtsc();
	va = pagr_entries[idx].va;
	spin_unlock_irqrestore(&pagr_lock, flags);

	spin_lock_irqsave(&pagr_queue_lock, flags);
	next = (pagr_queue_head + 1) % PAGR_QUEUE_SIZE;
	if (next == pagr_queue_tail) {
		spin_unlock_irqrestore(&pagr_queue_lock, flags);

		spin_lock_irqsave(&pagr_lock, flags);
		if (pagr_entries[idx].stamp == queue_entry.stamp) {
			pagr_entries[idx].predicted = false;
			__clear_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
		}
		spin_unlock_irqrestore(&pagr_lock, flags);

		pagr_stat_inc(PAGR_STAT_QUEUE_FULL);
		printk_ratelimited("PAGR: drop prediction queue full pfn=%lx va=%lx from_nid=%d to_nid=%d\n",
				   pfn, va, src_nid, target_nid);
		pagr_debug_maybe_dump("queue_full");
		return NUMA_NO_NODE;
	}

	pagr_prediction_queue[pagr_queue_head] = queue_entry;
	pagr_queue_head = next;
	spin_unlock_irqrestore(&pagr_queue_lock, flags);
	printk_ratelimited("PAGR: queued prediction pfn=%lx va=%lx from_nid=%d to_nid=%d\n",
			   pfn, va, src_nid, target_nid);
	pagr_stat_inc(PAGR_STAT_QUEUE_ACCEPTED);
	pagr_debug_maybe_dump("queue");

	return src_nid;
}

bool pagr_predictions_pending(void)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&pagr_queue_lock, flags);
	pending = pagr_queue_head != pagr_queue_tail;
	spin_unlock_irqrestore(&pagr_queue_lock, flags);

	return pending;
}

static bool dequeue_pagr_prediction(int nid, struct pagr_queue_entry *out)
{
	unsigned long flags;
	unsigned int scans, max_scans;
	bool found = false;

	spin_lock_irqsave(&pagr_queue_lock, flags);
	max_scans = queue_count_locked();

	for (scans = 0; scans < max_scans; scans++) {
		struct pagr_queue_entry entry;
		unsigned int next;

		if (pagr_queue_tail == pagr_queue_head)
			break;

		entry = pagr_prediction_queue[pagr_queue_tail];
		pagr_prediction_queue[pagr_queue_tail].idx = PAGR_INVALID_ENTRY;
		pagr_prediction_queue[pagr_queue_tail].stamp = 0;
		pagr_queue_tail = (pagr_queue_tail + 1) % PAGR_QUEUE_SIZE;

		if (entry.src_nid == nid) {
			*out = entry;
			found = true;
			break;
		}

		pagr_stat_inc(PAGR_STAT_DEQUEUE_REQUEUED);
		next = (pagr_queue_head + 1) % PAGR_QUEUE_SIZE;
		if (next == pagr_queue_tail)
			continue;

		pagr_prediction_queue[pagr_queue_head] = entry;
		pagr_queue_head = next;
	}

	spin_unlock_irqrestore(&pagr_queue_lock, flags);
	if (!found)
		pagr_stat_inc(PAGR_STAT_DEQUEUE_EMPTY);
	return found;
}

static void forget_history_entry(unsigned int idx, unsigned long stamp,
				 struct page *page)
{
	unsigned long flags;

	spin_lock_irqsave(&pagr_lock, flags);

	if (idx < PAGR_ENTRY_TABLE_SIZE &&
	    pagr_entries[idx].page == page &&
	    pagr_entries[idx].stamp == stamp) {
		reset_entry(idx);
		__clear_bit(PAGR_PAGE_IN_HISTORY, &page[3].pagr_flags);
		__clear_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
	}

	spin_unlock_irqrestore(&pagr_lock, flags);
}

unsigned long process_pagr_predictions(pg_data_t *pgdat)
{
	unsigned long total_migrated = 0;
	unsigned int failed = 0;
	unsigned int processed = 0;

	pagr_stat_inc(PAGR_STAT_PROCESS_CALLS);

	if (!pgdat)
		return 0;

	while (processed < PAGR_MAX_MIGRATE_BATCH) {
		struct pagr_queue_entry queue_entry;
		struct page *page = NULL;
		unsigned long flags;
		unsigned long migrated;
		u64 start_cyc, move_diff;

		if (!dequeue_pagr_prediction(pgdat->node_id, &queue_entry))
			break;

		processed++;
		if (queue_entry.idx >= PAGR_ENTRY_TABLE_SIZE)
			continue;

		spin_lock_irqsave(&pagr_lock, flags);
		if (pagr_entries[queue_entry.idx].page &&
		    pagr_entries[queue_entry.idx].stamp == queue_entry.stamp) {
			page = pagr_entries[queue_entry.idx].page;
			pagr_entries[queue_entry.idx].predicted = false;
			__clear_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
		}

		spin_unlock_irqrestore(&pagr_lock, flags);

		if (!page) {
			pagr_stat_inc(PAGR_STAT_PROCESS_STALE);
			continue;
		}

		start_cyc = rdtsc();
		if (queue_entry.enqueue_cyc && start_cyc > queue_entry.enqueue_cyc) {
			WRITE_ONCE(pagr_mig_queue_time,
				   update_mig_time(READ_ONCE(pagr_mig_queue_time),
						   start_cyc - queue_entry.enqueue_cyc));
		}

		migrated = migrate_pagr_predicted_page(pgdat, page);
		total_migrated += migrated;
		if (migrated) {
			pagr_stat_inc(PAGR_STAT_PROCESS_MIGRATE_ENTRIES);
			pagr_stat_add(PAGR_STAT_PROCESS_MIGRATE_BASE_PAGES,
				      migrated);
		} else {
			failed++;
			pagr_stat_inc(PAGR_STAT_PROCESS_MIGRATE_FAILED);
		}

		move_diff = rdtsc() - start_cyc;
		if (move_diff) {
			WRITE_ONCE(pagr_mig_move_time,
				   update_mig_time(READ_ONCE(pagr_mig_move_time),
						   move_diff));
		}

		if (migrated) {
			forget_history_entry(queue_entry.idx,
					     queue_entry.stamp,
					     page);
		}
	}

	if (processed)
		pr_info_ratelimited("PAGR_BATCH node=%d entries=%u migrated_base=%lu failed_entries=%u pending=%u mig_queue=%llu mig_move=%llu\n",
				    pgdat->node_id, processed, total_migrated,
				    failed, (unsigned int)pagr_predictions_pending(),
				    READ_ONCE(pagr_mig_queue_time),
				    READ_ONCE(pagr_mig_move_time));

	if (!queue_count_snapshot() && READ_ONCE(pagr_mig_queue_time))
		WRITE_ONCE(pagr_mig_queue_time, 0);

	pagr_debug_maybe_dump("process");

	return total_migrated;
}

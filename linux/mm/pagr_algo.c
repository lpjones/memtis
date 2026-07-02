#include <linux/mm.h>
#include <linux/pagr.h>
#include <linux/page_ext.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/pagemap.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include "internal.h"

#define ABS(x) ((x) >= 0 ? (x) : -(x))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLIP(x, a, b) (MIN(MAX((x), (a)), (b)))

#define PAGR_SCALE 1000000ULL

static struct page *pagr_history[PAGR_HISTORY_SIZE];
static DEFINE_SPINLOCK(pagr_lock);

static u64 top_cyc = 2 * PAGR_SCALE, bot_cyc = 1 * PAGR_SCALE;
static u64 pagr_avg_dist = 1 * PAGR_SCALE;
static u64 pagr_bot_dist = 1 * PAGR_SCALE;

#define PAGR_DEC_FAST_NUM 10000ULL
#define PAGR_DEC_SLOW_NUM 200ULL
#define PAGR_DEC_DIST_NUM 100ULL

LIST_HEAD(pagr_promotion_queue);
static DEFINE_SPINLOCK(pagr_queue_lock);

static inline u64 update_top(u64 top, u64 val) {
    if (val < top) {
        return (PAGR_DEC_SLOW_NUM * val + (PAGR_SCALE - PAGR_DEC_SLOW_NUM) * top) / PAGR_SCALE;
    }
    return (PAGR_DEC_FAST_NUM * val + (PAGR_SCALE - PAGR_DEC_FAST_NUM) * top) / PAGR_SCALE;
}

static inline u64 update_bot(u64 bot, u64 val) {
    if (val < bot) {
        return (PAGR_DEC_FAST_NUM * val + (PAGR_SCALE - PAGR_DEC_FAST_NUM) * bot) / PAGR_SCALE;
    }
    return (PAGR_DEC_SLOW_NUM * val + (PAGR_SCALE - PAGR_DEC_SLOW_NUM) * bot) / PAGR_SCALE;
}

static u64 calc_distance(struct pagr_ext *a, struct pagr_ext *b) {
    u64 distance;
    u64 dist_clip;
    u64 cyc_diff = (a->cyc > b->cyc) ? (a->cyc - b->cyc) : (b->cyc - a->cyc);

    u64 cyc_diff_clip = CLIP(cyc_diff, bot_cyc / 10, top_cyc * 10);
    
    top_cyc = update_top(top_cyc, cyc_diff_clip);
    bot_cyc = update_bot(bot_cyc, cyc_diff_clip);

    if (top_cyc <= bot_cyc) return 0;
    
    distance = (cyc_diff_clip > bot_cyc) ? (cyc_diff_clip - bot_cyc) * PAGR_SCALE / (top_cyc - bot_cyc) : 0;
    
    dist_clip = CLIP(distance, pagr_bot_dist / 10, pagr_avg_dist * 10);
    pagr_bot_dist = update_bot(pagr_bot_dist, dist_clip);
    pagr_avg_dist = (PAGR_DEC_DIST_NUM * dist_clip + (PAGR_SCALE - PAGR_DEC_DIST_NUM) * pagr_avg_dist) / PAGR_SCALE;

    return distance;
}

static void update_neighbors(struct page *old_page_ptr, struct pagr_ext *old_page)
{
    int i, j;
    
    for (i = 0; i < PAGR_MAX_NEIGHBORS; i++) {
        old_page->distances[i] = (old_page->distances[i] * 11) / 10; // NEIGHBOR_DEC = 1.1
    }

    for (i = 0; i < PAGR_HISTORY_SIZE; i++) {
        struct page *cur_page_ptr = pagr_history[i];
        struct pagr_ext *cur_page;
        u64 distance;
        int furthest_idx = -1;
        unsigned int max_dist = 0;

        if (!cur_page_ptr || cur_page_ptr == old_page_ptr) continue;

        cur_page = get_pagr_ext(cur_page_ptr);
        if (!cur_page) continue;

        distance = calc_distance(old_page, cur_page);
        
        for (j = 0; j < PAGR_MAX_NEIGHBORS; j++) {
            if (old_page->neighbors[j] == page_to_pfn(cur_page_ptr)) {
                furthest_idx = j;
                old_page->distances[j] = 0;
                break;
            }
            if (old_page->neighbors[j] == 0) {
                furthest_idx = j;
                max_dist = ~0U; // Force selection
                break;
            }
            if (furthest_idx == -1 || old_page->distances[j] > max_dist) {
                furthest_idx = j;
                max_dist = old_page->distances[j];
            }
        }

        if (furthest_idx != -1 && (old_page->distances[furthest_idx] == 0 || distance < old_page->distances[furthest_idx] || max_dist == ~0U)) {
            old_page->neighbors[furthest_idx] = page_to_pfn(cur_page_ptr);
            old_page->distances[furthest_idx] = distance;
        }
    }
}

void pagr_add_page(struct page *page, unsigned long va, unsigned long cyc, unsigned long ip)
{
    struct pagr_ext *pext = get_pagr_ext(page);
    struct page *old_page_ptr = NULL;
    int old_idx = 0;
    u64 min_cyc = ~0ULL;
    int i;
    unsigned long flags;

    if (!pext) return;

    spin_lock_irqsave(&pagr_lock, flags);

    pext->va = va;
    pext->cyc = cyc;
    pext->ip = ip;
    pext->in_history = true;

    for (i = 0; i < PAGR_HISTORY_SIZE; i++) {
        struct page *p = pagr_history[i];
        struct pagr_ext *cext;
        if (!p) {
            pagr_history[i] = page;
            spin_unlock_irqrestore(&pagr_lock, flags);
            return;
        }

        cext = get_pagr_ext(p);
        if (cext && cext->cyc < min_cyc) {
            min_cyc = cext->cyc;
            old_page_ptr = p;
            old_idx = i;
        }
    }

    if (old_page_ptr) {
        struct pagr_ext *old_pext = get_pagr_ext(old_page_ptr);
        if (old_pext) {
            update_neighbors(old_page_ptr, old_pext);
            old_pext->in_history = false;
        }
        pagr_history[old_idx] = page;
    }

    spin_unlock_irqrestore(&pagr_lock, flags);
}

int pagr_predict_pages(struct page *page, struct page **out_predictions)
{
    struct pagr_ext *pext = get_pagr_ext(page);
    int count = 0;
    int i, d;
    unsigned long flags;
    struct pagr_ext *cur_pext = pext;
    u64 base_threshold;
    u64 threshold;

    if (!pext) return 0;

    spin_lock_irqsave(&pagr_lock, flags);

    base_threshold = pagr_bot_dist;
    // Simplification of skewness adjustment for kernel
    threshold = base_threshold;

    for (d = 0; d < PAGR_PREDICTION_DEPTH; d++) {
        int closest_idx = -1;
        u64 min_dist = ~0ULL;
        struct page *next_page;
        
        for (i = 0; i < PAGR_MAX_NEIGHBORS; i++) {
            if (cur_pext->distances[i] != 0 && cur_pext->distances[i] < threshold) {
                struct page *pred_page;
                struct pagr_ext *pred_pext;

                if (cur_pext->distances[i] < min_dist) {
                    closest_idx = i;
                    min_dist = cur_pext->distances[i];
                }
                
                pred_page = pfn_to_page(cur_pext->neighbors[i]);
                if (pred_page && count < PAGR_MAX_PREDICTIONS) {
                    pred_pext = get_pagr_ext(pred_page);
                    if (pred_pext && !pred_pext->pred) {
                        out_predictions[count++] = pred_page;
                    }
                }
            }
        }
        
        if (closest_idx == -1 || cur_pext->neighbors[closest_idx] == 0) break;
        next_page = pfn_to_page(cur_pext->neighbors[closest_idx]);
        if (!next_page) break;
        cur_pext = get_pagr_ext(next_page);
        if (!cur_pext) break;
    }

    spin_unlock_irqrestore(&pagr_lock, flags);
    return count;
}

void queue_pagr_prediction(struct page *page)
{
    struct pagr_ext *pext = get_pagr_ext(page);
    unsigned long flags;

    if (!pext || pext->pred) return;

    spin_lock_irqsave(&pagr_queue_lock, flags);
    if (!pext->pred) {
        pext->pred = true;
        list_add_tail(&pext->pagr_pred_list, &pagr_promotion_queue);
    }
    spin_unlock_irqrestore(&pagr_queue_lock, flags);
}

unsigned long process_pagr_predictions(struct list_head *out_list, pg_data_t *pgdat)
{
    unsigned long isolated = 0;
    unsigned long flags;
    struct pagr_ext *pext, *next;
    LIST_HEAD(local_queue);

    spin_lock_irqsave(&pagr_queue_lock, flags);
    list_splice_init(&pagr_promotion_queue, &local_queue);
    spin_unlock_irqrestore(&pagr_queue_lock, flags);

    list_for_each_entry_safe(pext, next, &local_queue, pagr_pred_list) {
        struct page *page = pext->page;
        
        list_del_init(&pext->pagr_pred_list);
        pext->pred = false;

        if (!page) continue;

        if (!trylock_page(page)) continue;

        if (PageLRU(page)) {
            // Isolate page from LRU and add to out_list
            struct lruvec *lruvec = mem_cgroup_page_lruvec(page);
            spin_lock_irq(&lruvec->lru_lock);
            if (PageLRU(page)) {
                ClearPageLRU(page);
                list_del(&page->lru);
                update_lru_size(lruvec, page_lru(page), page_zonenum(page), -thp_nr_pages(page));
                list_add(&page->lru, out_list);
                isolated++;
            }
            spin_unlock_irq(&lruvec->lru_lock);
        }
        
        unlock_page(page);
    }

    return isolated;
}
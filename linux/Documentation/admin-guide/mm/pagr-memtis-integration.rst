==========================
PAGR Integration in Memtis
==========================

This note documents how the PAGR data-tiering algorithm is wired into the
Memtis kernel tree under ``memtis/linux``. It is based on the current code in
this tree and uses source locations so the implementation can be audited or
changed later.

Short answer on metadata lifetime
=================================

PAGR metadata is not owned by a userspace allocation object. It is stored in
physical transparent huge page tail ``struct page`` fields and in global PAGR
algorithm tables. The code updates and partially clears that state in a few
places, but it does not have a general userspace deallocation hook that removes
all PAGR metadata when an application frees or unmaps memory.

Important details:

* Newly allocated PAGR-prepped THPs reset ``page[3].last_va``,
  ``page[3].last_cyc``, and ``page[3].last_ip`` in
  ``mm/pagr_core.c:238``.
* ``pagr_add_page()`` sets ``PAGR_PAGE_IN_HISTORY`` and optionally
  ``PAGR_PAGE_PREDICTED`` in ``page[3].pagr_flags`` in
  ``mm/pagr_algo.c:813``.
* A successfully processed predicted migration calls
  ``forget_history_entry()``, which clears the global table entry and clears
  both PAGR flag bits on the old page in ``mm/pagr_algo.c:1176``.
* Normal THP zap/unmap paths call ``uncharge_htmm_page()`` from
  ``mm/huge_memory.c:1641``, but the PAGR build of that function does not clear
  PAGR state; its useful body is under ``CONFIG_HTMM`` in
  ``mm/pagr_core.c:873``.
* ``pagr_flags`` is not reset in ``__prep_transhuge_page_for_htmm()`` for
  ``CONFIG_PAGR``. Unless the page goes through the predicted-migration cleanup
  path or is overwritten by later PAGR activity, those flag bits can persist in
  the physical page metadata across userspace deallocation and later reuse.

The practical conclusion is that PAGR history is best understood as sampled
physical-page history, not as per-userspace-allocation metadata. Queue entries
use a generation stamp and page pointer to avoid many stale-entry mistakes, but
there is no complete free-side invalidation pass for the PAGR table or tail-page
flags.

Build integration
=================

PAGR is compiled with its own ``CONFIG_PAGR`` option. The option is separate
from ``CONFIG_HTMM`` but depends on the same base capabilities: migration and
transparent huge pages.

Source: ``mm/Kconfig:900``

.. code-block:: c

   config HTMM
       bool "Enable hugepage-aware tiered memory management"
       depends on MIGRATION && TRANSPARENT_HUGEPAGE

   config PAGR
       bool "Enable PAGR metadata tracking"
       depends on MIGRATION && TRANSPARENT_HUGEPAGE
       default y

The Memtis build links either the HTMM objects or the PAGR objects depending on
the config option:

Source: ``mm/Makefile:133``

.. code-block:: make

   obj-$(CONFIG_HTMM) += htmm_sampler.o htmm_core.o htmm_migrater.o
   obj-$(CONFIG_PAGR) += pagr_sampler.o pagr_core.o pagr_migrater.o pagr_algo.o

The PAGR implementation is therefore mostly a parallel implementation of the
Memtis sampling, core metadata, and migration code. It reuses many Memtis names
and interfaces, which is why runtime controls still contain ``htmm`` in their
names.

Public interface and constants
==============================

The PAGR public header is ``include/linux/pagr.h``. It defines the in-kernel
interface used by the sampler, core, and migrater, plus the graph-tracing record
format.

Source: ``include/linux/pagr.h:9``

.. code-block:: c

   #define PAGR_HISTORY_SIZE       16
   #define PAGR_ENTRY_TABLE_SIZE   1024
   #define PAGR_MAX_NEIGHBORS      4
   #define PAGR_PREDICTION_DEPTH   16
   #define PAGR_MAX_PREDICTIONS    (PAGR_MAX_NEIGHBORS * PAGR_PREDICTION_DEPTH)
   #define PAGR_MAX_PREDICTIONS_PER_SAMPLE 8
   #define PAGR_QUEUE_SIZE         4096
   #define PAGR_MAX_MIGRATE_BATCH  64

Source: ``include/linux/pagr.h:22``

.. code-block:: c

   #define PAGR_PAGE_IN_HISTORY    0
   #define PAGR_PAGE_PREDICTED     1

Source: ``include/linux/pagr.h:201``

.. code-block:: c

   extern void pagr_add_page(struct page *page, unsigned long va,
                             unsigned long cyc, unsigned long ip);
   extern int pagr_predict_pages(struct page *page,
                                 struct page **out_predictions);
   extern int queue_pagr_prediction(struct page *page);
   extern unsigned long process_pagr_predictions(pg_data_t *pgdat);
   extern bool pagr_predictions_pending(void);
   extern unsigned long migrate_pagr_predicted_page(pg_data_t *pgdat,
                                                    struct page *page);

The graph-tracing ABI is also declared there. The graph records contain source
and destination virtual addresses, PFNs, cycle/IP samples, neighbor distance,
threshold information, entry indices, and an insert/refresh/replace event.

Source: ``include/linux/pagr.h:144``

.. code-block:: c

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

Metadata placement in struct page
=================================

PAGR stores its per-page state directly in tail ``struct page`` slots for
compound THPs. The third tail page stores the last sampled tuple and PAGR flags.
The fourth and later tail pages can hold ``pginfo_t`` through ``pagr_info``.

Source: ``include/linux/mm_types.h:181``

.. code-block:: c

   #ifdef CONFIG_PAGR
       struct {    /* Third~ tail pages of compound page */
           unsigned long __compound_pad_1; /* compound_head */
           union {
               struct {    /* Third tail page of compound page */
                   unsigned long last_va;
                   unsigned long last_cyc;
                   unsigned long last_ip;
                   unsigned long pagr_flags;
               };
               struct {    /* Fourth~ tail pages */
                   pginfo_t pagr_info;
               };
           };
       };
   #endif

The process-level enable bit is shared with Memtis and remains named
``htmm_enabled``:

Source: ``include/linux/mm_types.h:628``

.. code-block:: c

   #if defined(CONFIG_HTMM) || defined(CONFIG_PAGR)
       bool htmm_enabled;
   #endif

PAGR also reuses the per-node Memtis migrater fields in ``pglist_data``:

Source: ``include/linux/mmzone.h:889``

.. code-block:: c

   #if defined(CONFIG_HTMM) || defined(CONFIG_PAGR)
       struct cftype *memcg_htmm_file;
       struct task_struct  *kmigraterd;
       struct list_head    kmigraterd_head;
       spinlock_t          kmigraterd_lock;
       wait_queue_head_t   kmigraterd_wait;
   #endif

Runtime controls
================

Cgroup enablement uses the existing Memtis control file,
``memory.htmm_enabled``. When enabled, the code starts ``kmigraterd`` and adds
the memcg to each node's migrater list. When disabled, it stops the migrater and
removes the memcg.

Source: ``mm/memcontrol.c:7592``

.. code-block:: c

   if (sysfs_streq(buf, "enabled"))
       memcg->htmm_enabled = true;
   else if (sysfs_streq(buf, "disabled"))
       memcg->htmm_enabled = false;
   else
       return -EINVAL;

   if (memcg->htmm_enabled) {
       kmigraterd_init();
   } else {
       kmigraterd_stop();
   }

Sampling starts through the existing ``htmm_start`` syscall hook in
``kernel/events/core.c``. With either ``CONFIG_HTMM`` or ``CONFIG_PAGR``, it
starts ``ksamplingd``; ``htmm_end`` stops it.

Source: ``kernel/events/core.c:12518``

.. code-block:: c

   SYSCALL_DEFINE2(htmm_start, pid_t, pid, int, node)
   {
       ksamplingd_init(pid, node);
       return 0;
   }

   SYSCALL_DEFINE1(htmm_end, pid_t, pid)
   {
       ksamplingd_exit();
       return 0;
   }

PAGR-specific knobs live in the same ``/sys/kernel/mm/htmm`` group as the
Memtis knobs. Defaults are defined in ``mm/mempolicy.c``:

Source: ``mm/mempolicy.c:3053``

.. code-block:: c

   unsigned int pagr_fast_threshold_min_percent = 5;
   unsigned int pagr_fast_threshold_power = 2;
   unsigned int pagr_fast_threshold_min_samples = 1024;
   unsigned int pagr_max_predictions_per_sample = 4;
   unsigned int pagr_trace_enabled = 1;
   unsigned int pagr_graph_enabled = 1;
   unsigned int pagr_graph_sample_interval = 8;
   unsigned int pagr_debug_interval_ms = 0;
   unsigned int pagr_verbose = 0;

They are exposed with a small unsigned-integer sysfs helper:

Source: ``mm/mempolicy.c:3619``

.. code-block:: c

   #define PAGR_UINT_SYSFS_ATTR(name)                                      \
   static ssize_t name##_show(struct kobject *kobj,                        \
                              struct kobj_attribute *attr, char *buf)      \
   {                                                                       \
       return sysfs_emit(buf, "%u\n", READ_ONCE(name));                   \
   }                                                                       \
                                                                           \
   static ssize_t name##_store(struct kobject *kobj,                       \
                               struct kobj_attribute *attr,                \
                               const char *buf, size_t count)              \
   {                                                                       \
       int err;                                                            \
       unsigned int value;                                                 \
                                                                           \
       err = kstrtouint(buf, 10, &value);                                  \
       if (err)                                                            \
           return err;                                                     \
                                                                           \
       WRITE_ONCE(name, value);                                            \
       return count;                                                       \
   }

The attributes are added to the existing ``htmm_attrs`` list in
``mm/mempolicy.c:3674`` and the kobject is created as ``htmm`` under
``mm_kobj`` in ``mm/mempolicy.c:3690``.

Allocation and page preparation
===============================

PAGR uses Memtis's tier-aware allocation hook in ``alloc_pages_vma()``. If both
the ``mm`` and memcg are enabled, allocation checks the memcg's per-node page
cap. If the preferred node is full, it walks to a demotion node, allocates with
``__GFP_THISNODE``, and wakes the migrater when demotion is needed.

Source: ``mm/mempolicy.c:2110``

.. code-block:: c

   if (vma->vm_mm && vma->vm_mm->htmm_enabled) {
       struct mem_cgroup *memcg = mem_cgroup_from_task(p);
       int nid = pol->mode == MPOL_PREFERRED ? first_node(pol->nodes) : node;
       int orig_nid = nid;

       if (!memcg || !memcg->htmm_enabled)
           goto use_default_pol;

       while (max_nr_pages <= (get_nr_lru_pages_node(memcg, pgdat) +
                               nr_pages)) {
           if ((nid = next_demotion_node(nid)) == NUMA_NO_NODE) {
               nid = first_memory_node;
               break;
           }
       }

       if (orig_nid != nid) {
           WRITE_ONCE(memcg->nodeinfo[orig_nid]->need_demotion, true);
           kmigraterd_wakeup(orig_nid);
       }

       page = __alloc_pages_node(nid, gfp | __GFP_THISNODE, order);
       goto out;
   }

New anonymous THPs are prepared through ``prep_transhuge_page_for_htmm()`` from
``mm/huge_memory.c``. Under ``CONFIG_PAGR``, this initializes the last sampled
tuple to zero. It does not clear ``pagr_flags``.

Source: ``mm/huge_memory.c:800``

.. code-block:: c

   page = alloc_hugepage_vma(gfp, vma, haddr, HPAGE_PMD_ORDER);
   if (unlikely(!page)) {
       count_vm_event(THP_FAULT_FALLBACK);
       return VM_FAULT_FALLBACK;
   }
   #if defined(CONFIG_HTMM) || defined(CONFIG_PAGR)
       prep_transhuge_page_for_htmm(vma, page);
   #else
       prep_transhuge_page(page);
   #endif

Source: ``mm/pagr_core.c:238``

.. code-block:: c

   void __prep_transhuge_page_for_htmm(struct mm_struct *mm, struct page *page)
   {
   #ifdef CONFIG_HTMM
       ...
   #elif defined(CONFIG_PAGR)
       page[3].last_va = 0;
       page[3].last_cyc = 0;
       page[3].last_ip = 0;
       /* No SetPageHtmm for CONFIG_PAGR */
   #endif

       ClearPageActive(page);
   }

Sampling flow
=============

PAGR sampling is implemented in ``mm/pagr_sampler.c``. It opens PEBS events
system-wide on the selected CPU mask and reads perf ring buffers in
``ksamplingd``. For every valid ``PERF_RECORD_SAMPLE``, it creates a compact
record, optionally logs it to ``/tmp/memtis_pebs_trace.bin``, and calls
``update_pginfo()``.

Source: ``mm/pagr_sampler.c:466``

.. code-block:: c

   case PERF_RECORD_SAMPLE: {
       struct pebs_rec rec;

       if (ph.size < sizeof(he) ||
           !htmm_perf_rb_copy(rb, tail, &he, sizeof(he)) ||
           !valid_va(he.addr)) {
           break;
       }

       rec.cyc = rdtsc();
       rec.va = he.addr & HPAGE_PMD_MASK;
       rec.ip = he.ip;
       rec.cpu = cpu;
       rec.evt = event;

       update_pginfo(he.pid, he.addr, event, rec.cyc, rec.ip);

       if (event == DRAMREAD)
           pagr_note_access(true);
       else if (event == CXLREAD || event == NVMREAD)
           pagr_note_access(false);
   }

The sampler also maintains a one-second fast-vs-slow sample window. It prints
sample period, CPU time, hit ratio, promotions, demotions, fast accesses, and
slow accesses, then resets the PAGR threshold-scaling counters.

Source: ``mm/pagr_sampler.c:601``

.. code-block:: c

   pr_info("sample_period: %lu || cputime: %lu || hit_ratio: %lu || promoted: %lu || demoted: %lu || fast: %lu || slow: %lu\n",
           get_sample_period(sample_period), trace_cputime, hr,
           promoted_per_sec, demoted_per_sec, hr_dram, hr_nvm);

   hr_dram = hr_nvm = 0;
   pagr_reset_access_counters();

Access handling and prediction
==============================

``update_pginfo()`` validates the sampled address, finds the VMA, checks that
it is migratable, walks the page table, and handles THP samples in
``__update_pmd_pginfo()``.

Source: ``mm/pagr_core.c:1711``

.. code-block:: c

   void update_pginfo(pid_t pid, unsigned long address, enum events e,
                      unsigned long cyc, unsigned long ip)
   {
       struct pid *pid_struct = find_get_pid(pid);
       struct task_struct *p = pid_struct ? pid_task(pid_struct, PIDTYPE_PID) : NULL;
       struct mm_struct *mm = p ? p->mm : NULL;

       if (!mmap_read_trylock(mm))
           goto put_task;

       vma = find_vma(mm, address);
       if (unlikely(!vma))
           goto mmap_unlock;

       if (!vma->vm_mm || !vma_migratable(vma) ||
           (vma->vm_file && (vma->vm_flags & (VM_READ | VM_WRITE)) == (VM_READ)))
           goto mmap_unlock;

       ret = __update_pginfo(vma, address, cyc, ip);
       ...
   }

For THPs, the PMD path records the last sampled page-aligned virtual address,
cycle count, and instruction pointer on the third tail page, then calls
``update_huge_page()``.

Source: ``mm/pagr_core.c:1407``

.. code-block:: c

   #ifdef CONFIG_PAGR
       page[3].last_va = address & HPAGE_PMD_MASK;
   #else
       page[3].last_va = address;
   #endif
       page[3].last_cyc = cyc;
       page[3].last_ip = ip;

       hot = update_huge_page(vma, pmd, page, address);

The PAGR branch of ``update_huge_page()`` converts the sample into graph
history, predicts future pages, queues predictions, wakes the migrater for each
accepted prediction, and optionally writes prediction records.

Source: ``mm/pagr_core.c:1249``

.. code-block:: c

   page = compound_head(page);
   address &= HPAGE_PMD_MASK;
   page[3].last_va = address;
   pagr_add_page(page, address, page[3].last_cyc, page[3].last_ip);
   nr_predictions = pagr_predict_pages(page, predictions);

   for (i = 0; i < nr_predictions; i++) {
       pred_nid = queue_pagr_prediction(predictions[i]);
       if (pred_nid == NUMA_NO_NODE)
           continue;
       nr_queued++;
       kmigraterd_wakeup(pred_nid);
       ...
   }

PAGR algorithm state
====================

The algorithm state lives in ``mm/pagr_algo.c``. It has a fixed entry table, a
small history list, and a prediction queue.

Source: ``mm/pagr_algo.c:38``

.. code-block:: c

   struct pagr_entry {
       struct page *page;
       unsigned long va;
       unsigned long cyc;
       unsigned long ip;
       unsigned long stamp;
       bool predicted;
       struct pagr_neighbor neighbors[PAGR_MAX_NEIGHBORS];
   };

   static struct pagr_entry pagr_entries[PAGR_ENTRY_TABLE_SIZE];
   static unsigned int pagr_history[PAGR_HISTORY_SIZE];
   static unsigned int pagr_history_count;
   static struct pagr_queue_entry pagr_prediction_queue[PAGR_QUEUE_SIZE];

Each sampled THP is inserted or refreshed in the entry table by
``pagr_add_page()``. The function stores the last sample tuple both in the
global entry and in the page tail metadata, records the entry in the history
window, and sets the in-history flag.

Source: ``mm/pagr_algo.c:813``

.. code-block:: c

   idx = find_entry(page);
   if (idx < 0)
       idx = alloc_entry();

   entry = &pagr_entries[idx];
   if (!entry->page) {
       clear_entry_neighbors(entry);
       entry->stamp = new_entry_stamp();
       entry->predicted = false;
   }

   entry->page = page;
   entry->va = va;
   entry->cyc = cyc;
   entry->ip = ip;

   record_history_sample(idx, log_graph ? graph_records : NULL,
                         &nr_graph_records, ARRAY_SIZE(graph_records));

   page[3].last_va = va;
   page[3].last_cyc = cyc;
   page[3].last_ip = ip;
   __set_bit(PAGR_PAGE_IN_HISTORY, &page[3].pagr_flags);

When the history window wraps, ``record_history_sample()`` updates the outgoing
entry's neighbors before replacing it with the new index. Neighbor distance is
computed from normalized VA, cycle, and IP deltas.

Source: ``mm/pagr_algo.c:538``

.. code-block:: c

   va_diff = normalize_diff(PAGR_ABS_DIFF(a->va, b->va),
                            &top_va, &bot_va);
   cyc_diff = normalize_diff(PAGR_ABS_DIFF(a->cyc, b->cyc),
                             &top_cyc, &bot_cyc);
   ip_diff = normalize_diff(PAGR_ABS_DIFF(a->ip, b->ip),
                            &top_ip, &bot_ip);

   distance = va_diff + cyc_diff + ip_diff;

Aggressiveness tuning
=====================

The current integration includes a PACT-like fast-tier hit-rate factor. The
sampler calls ``pagr_note_access(true)`` for DRAM samples and
``pagr_note_access(false)`` for NVM/CXL samples. ``pagr_percent_fast_factor()``
then scales the distance threshold by ``1 - percent_fast^N``, with a minimum
factor controlled by ``pagr_fast_threshold_min_percent``.

Source: ``mm/pagr_algo.c:122``

.. code-block:: c

   static u64 pagr_percent_fast_factor(void)
   {
       u64 fast = atomic64_read(&pagr_fast_samples);
       u64 slow = atomic64_read(&pagr_slow_samples);
       u64 samples = fast + slow;

       if (samples < READ_ONCE(pagr_fast_threshold_min_samples))
           return PAGR_SCALE;

       pf = div64_u64(fast * PAGR_SCALE, samples);
       pf_pow = pagr_pow_scaled(pf, READ_ONCE(pagr_fast_threshold_power));

       return max_t(u64, PAGR_SCALE - pf_pow, min_factor);
   }

Source: ``mm/pagr_algo.c:550``

.. code-block:: c

   dist_clip = PAGR_CLIP(distance, pagr_bot_dist / 10, pagr_avg_dist * 10);
   dist_scaled = mul_u64_u64_div_u64(dist_clip,
                                     pagr_percent_fast_factor(),
                                     PAGR_SCALE);
   pagr_bot_dist = update_bot(pagr_bot_dist, dist_scaled);

This is the main guard against over-aggressive PAGR promotion: as most sampled
accesses hit the fast tier, the threshold decays toward its configured lower
bound; as slow-tier accesses dominate, the factor approaches one and original
PAGR behavior is preserved.

Prediction queue and migration
==============================

``pagr_predict_pages()`` walks neighbor edges up to
``PAGR_PREDICTION_DEPTH`` and selects predicted pages whose distance is below
the threshold, whose time gap exceeds current migration queue/move time, and
whose source node can be promoted.

Source: ``mm/pagr_algo.c:897``

.. code-block:: c

   threshold = pagr_bot_dist;
   mig_time = READ_ONCE(pagr_mig_queue_time) +
              READ_ONCE(pagr_mig_move_time);

   for (depth = 0; depth < PAGR_PREDICTION_DEPTH; depth++) {
       ...
       if (neighbor->distance >= threshold)
           continue;
       if (neighbor->time_diff + total_time_diff <= mig_time)
           continue;
       if (pred_entry->predicted)
           continue;
       if (!pagr_prediction_target(pred_page, NULL, NULL))
           continue;

       selected[count] = pred_idx;
       out_predictions[count++] = pred_page;
   }

``queue_pagr_prediction()`` records the PAGR entry index, entry generation
stamp, source node, target node, and enqueue timestamp. The generation stamp is
important because the global entry table can evict and reuse slots.

Source: ``mm/pagr_algo.c:1029``

.. code-block:: c

   pagr_entries[idx].predicted = true;
   __set_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
   queue_entry.idx = idx;
   queue_entry.stamp = pagr_entries[idx].stamp;
   queue_entry.src_nid = src_nid;
   queue_entry.target_nid = target_nid;
   queue_entry.enqueue_cyc = rdtsc();

``kmigraterd`` processes prediction batches before and after normal Memtis LRU
work. The promotion worker waits either for the normal promotion period or for
new PAGR predictions.

Source: ``mm/pagr_migrater.c:1183``

.. code-block:: c

   for ( ; ; ) {
       if (kthread_should_stop())
           break;

       process_pagr_predictions(pgdat);
       ...
       process_pagr_predictions(pgdat);

       if (need_lowertier_promotion(pgdat, memcg)) {
           promote_node(pgdat, memcg);
       }

       wait_event_interruptible_timeout(pgdat->kmigraterd_wait,
           kthread_should_stop() || pagr_predictions_pending(),
           msecs_to_jiffies(htmm_promotion_period_in_ms));
   }

``process_pagr_predictions()`` validates the queued entry by checking that the
entry pointer still exists and the stamp still matches. It clears the predicted
flag before attempting migration, updates moving-time estimates, and removes
history only after a successful migration.

Source: ``mm/pagr_algo.c:1219``

.. code-block:: c

   spin_lock_irqsave(&pagr_lock, flags);
   if (pagr_entries[queue_entry.idx].page &&
       pagr_entries[queue_entry.idx].stamp == queue_entry.stamp) {
       page = pagr_entries[queue_entry.idx].page;
       pagr_entries[queue_entry.idx].predicted = false;
       __clear_bit(PAGR_PAGE_PREDICTED, &page[3].pagr_flags);
   }
   spin_unlock_irqrestore(&pagr_lock, flags);

   migrated = migrate_pagr_predicted_page(pgdat, page);

   if (migrated) {
       forget_history_entry(queue_entry.idx, queue_entry.stamp, page);
   }

The actual predicted migration is performed in ``mm/pagr_migrater.c`` using the
normal kernel ``migrate_pages()`` machinery with ``MR_NUMA_MISPLACED``. The
page is promoted from the slow tier to the fast tier if it is still on the
expected source node and still migratable.

Source: ``mm/pagr_migrater.c:362``

.. code-block:: c

   page = compound_head(page);
   src_nid = page_to_nid(page);
   va = page[3].last_va;

   if (src_nid != pgdat->node_id)
       return 0;

   target_nid = htmm_cxl_mode ? 0 : next_promotion_node(pgdat->node_id);
   if (target_nid == NUMA_NO_NODE || target_nid == pgdat->node_id)
       return 0;

   migrate_rc = migrate_pages(&migrate_list, alloc_migrate_page, NULL,
       target_nid, MIGRATE_ASYNC, MR_NUMA_MISPLACED, &nr_succeeded);

Migration metadata caveat
=========================

``copy_transhuge_pginfo()`` has a PAGR branch that copies
``last_va/last_cyc/last_ip`` from the old THP to the new THP.

Source: ``mm/pagr_core.c:340``

.. code-block:: c

   #elif defined(CONFIG_PAGR)
       newpage[3].last_va = page[3].last_va;
       newpage[3].last_cyc = page[3].last_cyc;
       newpage[3].last_ip = page[3].last_ip;
       /* No SetPageHtmm for CONFIG_PAGR */
   #endif

However, the generic migration call site is guarded only by ``CONFIG_HTMM``:

Source: ``mm/migrate.c:725``

.. code-block:: c

   copy_page_owner(page, newpage);
   #ifdef CONFIG_HTMM
       if (PageTransHuge(page))
           copy_transhuge_pginfo(page, newpage);
   #endif

So if the kernel is built with ``CONFIG_PAGR=y`` and ``CONFIG_HTMM`` unset,
generic migration does not call the PAGR metadata copy helper. The PAGR-specific
allocator for migration does prepare the new THP:

Source: ``mm/pagr_migrater.c:317``

.. code-block:: c

   if (thp_migration_supported() && PageTransHuge(page)) {
       mask |= GFP_TRANSHUGE_LIGHT;
       newpage = __alloc_pages_node(nid, mask, HPAGE_PMD_ORDER);

       prep_transhuge_page(newpage);
       __prep_transhuge_page_for_htmm(NULL, newpage);
   }

That means the new page is initialized, but the old PAGR last-sample tuple is
not necessarily preserved by generic migration in a PAGR-only build. The
predicted-migration path already calls ``forget_history_entry()`` after success,
so the global PAGR history should not keep pointing at the old migrated page in
that success case.

Graphing and trace output
=========================

When tracing is enabled, PAGR writes binary PEBS, prediction, promotion, and
demotion traces under ``/tmp``. When graphing is enabled, it writes
``/tmp/memtis_pagr_graph.bin`` with a ``pagr_graph_header`` followed by
``pagr_graph_record`` entries.

Source: ``mm/pagr_core.c:44``

.. code-block:: c

   if (READ_ONCE(pagr_trace_enabled)) {
       memtis_pred_file = filp_open("/tmp/memtis_pred.bin",
                                    O_WRONLY | O_CREAT | O_TRUNC, 0600);
       memtis_promote_file = filp_open("/tmp/memtis_promote.bin",
                                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
       memtis_demote_file = filp_open("/tmp/memtis_demote.bin",
                                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
   }

   if (READ_ONCE(pagr_graph_enabled)) {
       memtis_pagr_graph_file = filp_open("/tmp/memtis_pagr_graph.bin",
                                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
   }

Graph records are generated while neighbor edges are inserted, refreshed, or
replaced. ``pagr_graph_sample_interval`` controls how often graph updates are
logged.

Source: ``mm/pagr_algo.c:862``

.. code-block:: c

   if (READ_ONCE(pagr_graph_enabled)) {
       graph_interval = max_t(unsigned int,
                              READ_ONCE(pagr_graph_sample_interval), 1);
       log_graph = (++pagr_graph_updates % graph_interval) == 0;
   }

   record_history_sample(idx, log_graph ? graph_records : NULL,
                         &nr_graph_records, ARRAY_SIZE(graph_records));

Deallocation and stale metadata
===============================

The deallocation behavior is the biggest correctness question for PAGR.

There are three separate kinds of state:

1. Tail-page last-sample tuple:
   ``page[3].last_va``, ``page[3].last_cyc``, ``page[3].last_ip``.
2. Tail-page PAGR flags:
   ``page[3].pagr_flags`` bits ``PAGR_PAGE_IN_HISTORY`` and
   ``PAGR_PAGE_PREDICTED``.
3. Global algorithm state:
   ``pagr_entries[]``, ``pagr_history[]``, and ``pagr_prediction_queue[]``.

On normal userspace unmap of a THP, the zap path calls ``uncharge_htmm_page()``
when either HTMM or PAGR is configured:

Source: ``mm/huge_memory.c:1639``

.. code-block:: c

   if (pmd_present(orig_pmd)) {
       page = pmd_page(orig_pmd);
   #if defined(CONFIG_HTMM) || defined(CONFIG_PAGR)
       uncharge_htmm_page(page, get_mem_cgroup_from_mm(vma->vm_mm));
   #endif
       page_remove_rmap(page, true);
   }

But the PAGR version of ``uncharge_htmm_page()`` does not clear PAGR metadata.
The only body that updates counters is under ``CONFIG_HTMM``:

Source: ``mm/pagr_core.c:873``

.. code-block:: c

   void uncharge_htmm_page(struct page *page, struct mem_cgroup *memcg)
   {
       unsigned int nr_pages = thp_nr_pages(page);
   #ifdef CONFIG_HTMM
       unsigned int idx;
       int i;
   #endif

       if (!memcg || !memcg->htmm_enabled)
           return;

       page = compound_head(page);
       if (nr_pages != 1) {
   #ifdef CONFIG_HTMM
           struct page *meta = get_meta_page(page);
           ...
   #endif
       }
   }

Base-page unmap has an even stronger HTMM guard in ``mm/memory.c:1368`` and
does not call this hook in a PAGR-only build.

The explicit PAGR cleanup path is tied to predicted migration success:

Source: ``mm/pagr_algo.c:1176``

.. code-block:: c

   static void forget_history_entry(unsigned int idx, unsigned long stamp,
                                    struct page *page)
   {
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

The global table also removes references when entries are evicted or reset:

Source: ``mm/pagr_algo.c:569``

.. code-block:: c

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

That reset path protects PAGR from table-slot reuse, but it is not called by
normal userspace free/unmap. Therefore stale state can remain until one of these
events happens:

* the same physical page is sampled again and ``pagr_add_page()`` refreshes the
  entry and last-sample tuple;
* the table entry is evicted by ``alloc_entry()`` and ``reset_entry()``;
* a queued predicted migration succeeds and ``forget_history_entry()`` clears
  the entry and page flags;
* a new THP allocation prepares the physical page and clears the last-sample
  tuple, though not ``pagr_flags``.

If strict deallocation cleanup is desired, add a PAGR-specific cleanup helper
that:

* clears ``page[3].last_va``, ``page[3].last_cyc``, ``page[3].last_ip``, and
  ``page[3].pagr_flags``;
* finds and resets any matching ``pagr_entries[]`` entry under ``pagr_lock``;
* invalidates queued predictions by using the existing entry stamp mechanism, or
  explicitly removes matching queue entries under ``pagr_queue_lock``;
* is called from the THP zap/free path under ``CONFIG_PAGR``.

That would make PAGR metadata lifetime match userspace allocation lifetime more
closely than it does today.

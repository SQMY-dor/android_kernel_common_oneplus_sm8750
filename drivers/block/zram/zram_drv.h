/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ZRAM_DRV_H_
#define _ZRAM_DRV_H_

#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/percpu_counter.h>
#include <linux/workqueue.h>
#include <linux/zsmalloc.h>
#include <linux/crypto.h>
#include <linux/list_lru.h>

#include "zcomp.h"

#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define ZRAM_LOGICAL_BLOCK_SHIFT 12
#define ZRAM_LOGICAL_BLOCK_SIZE	(1 << ZRAM_LOGICAL_BLOCK_SHIFT)
#define ZRAM_SECTOR_PER_LOGICAL_BLOCK	\
	(1 << (ZRAM_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))


/*
 * ZRAM is mainly used for memory efficiency so we want to keep memory
 * footprint small and thus squeeze size and zram pageflags into a flags
 * member. The lower ZRAM_FLAG_SHIFT bits is for object size (excluding
 * header), which cannot be larger than PAGE_SIZE (requiring PAGE_SHIFT
 * bits), the higher bits are for zram_pageflags.
 *
 * We use BUILD_BUG_ON() to make sure that zram pageflags don't overflow.
 */
#define ZRAM_FLAG_SHIFT (PAGE_SHIFT + 1)

/* Only 2 bits are allowed for comp priority index */
#define ZRAM_COMP_PRIORITY_MASK	0x3

/* Flags for zram pages (table[page_no].flags) */
enum zram_pageflags {
	/* zram slot is locked */
	ZRAM_LOCK = ZRAM_FLAG_SHIFT,
	ZRAM_SAME,	/* Page consists the same element */
	ZRAM_WB,	/* page is stored on backing_device */
	ZRAM_PP_SLOT,	/* Selected for post-processing */
	ZRAM_HUGE,	/* Incompressible page */
	ZRAM_IDLE,	/* not accessed page since last idle marking */
	ZRAM_INCOMPRESSIBLE, /* none of the algorithms could compress it */

	ZRAM_COMP_PRIORITY_BIT1, /* First bit of comp priority index */
	ZRAM_COMP_PRIORITY_BIT2, /* Second bit of comp priority index */

	ZRAM_PAGE_ANON,		/* 匿名页 */
	ZRAM_PAGE_FILE,		/* 文件页 */
	ZRAM_PAGE_DIRTY,	/* 脏页 */
	ZRAM_REFERENCED,	/* shrinker second-chance hint */
	ZRAM_STATE_MIGRATING,	/* page is being migrated on backing device */

	__NR_ZRAM_PAGEFLAGS,
};

/*-- Data structures */

/* Allocated for each disk page */
struct zram_table_entry {
	unsigned long handle;
	unsigned long flags;
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	ktime_t ac_time;
#endif
#ifdef	CONFIG_ZRAM_WRITEBACK
	struct list_head lru;
	u8 wb_nr_pages;
	u8 migration_count;
	u16 memcg_id;
#endif
};

#ifdef CONFIG_ZRAM_WRITEBACK
#define ZRAM_WB_CLUSTER_SHIFT	6
#define ZRAM_WB_CLUSTER_SIZE	(1UL << ZRAM_WB_CLUSTER_SHIFT)
#define ZRAM_WB_CLUSTER_MASK	(ZRAM_WB_CLUSTER_SIZE - 1)

#define BATCH_SIZE 64
#define WINDOW_RADIUS 8
#define MIN_AGGREGATE 4
#define ZRAM_SHADOW_CACHE_TTL	(2 * HZ)
#define ZRAM_SHADOW_CACHE_TTL_MIN	(HZ / 2)
#define ZRAM_SHADOW_CACHE_TTL_MAX	(10 * HZ)
#define ZRAM_SHADOW_CACHE_TTL_GROW_STEP	(HZ / 2)
#define ZRAM_SHADOW_CACHE_TTL_SHRINK_STEP	(HZ)
#define ZRAM_SHADOW_HIT_WINDOW	64
#define ZRAM_GC_PERIODIC_INTERVAL	(15 * HZ)
#define ZRAM_GC_PERIODIC_PAGES	16
#define ZRAM_GC_MAX_SCAN_CLUSTERS	128

enum zram_shadow_cache_state {
	ZRAM_SHADOW_CLEAN = 0,
	ZRAM_SHADOW_HIT,
};

enum zram_wb_cluster_state {
	ZRAM_WB_CLUSTER_CLEAN = 0,
	ZRAM_WB_CLUSTER_DIRTY_FREE,
};

struct zram_shadow_cache {
	struct list_head node;
	struct timer_list timer;
	struct zram *zram;
	struct page **pages;
	u32 *indexes;
	unsigned long cluster_base;
	unsigned long expires_at;
	unsigned long ttl_jiffies;
	unsigned long state_bitmap;
	u32 nr_pages;
	u32 age_seq;
	u32 bytes;
};

struct zram_shadow_prefetch {
	struct list_head node;
	struct work_struct work;
	struct zram *zram;
	unsigned long cluster_base;
	u32 nr_pages;
};
#endif

struct zram_stats {
	/* hot percpu */
	struct percpu_counter compr_data_size;
	struct percpu_counter same_pages;
	struct percpu_counter huge_pages;
	struct percpu_counter pages_stored;
#ifdef CONFIG_ZRAM_WRITEBACK
	struct percpu_counter bd_count;
	struct percpu_counter bd_reads;
	struct percpu_counter bd_writes;
#endif

	/* atomics */
	atomic_long_t max_used_pages;
	atomic64_t failed_reads;
	atomic64_t failed_writes;
	atomic64_t notify_free;
	atomic64_t huge_pages_since;
	atomic64_t writestall;
	atomic64_t miss_free;
#ifdef CONFIG_ZRAM_WRITEBACK
	atomic64_t written_back_pages;
	atomic64_t reject_reclaim_fail;
	atomic64_t prefetch_total;
	atomic64_t prefetch_hits;
#endif
};

#ifdef CONFIG_ZRAM_WRITEBACK
struct zram_wb_state {
	struct zram *owner;
	struct file *backing_dev;
	struct block_device *bdev;
	unsigned long *bitmap;
	unsigned long *dirty_free_bitmap;
	unsigned long nr_pages;
	unsigned long reclaim_threshold;
	unsigned long shadow_ttl_jiffies;
	unsigned long shadow_last_hit_rate;
	unsigned long gc_scan_cursor;
	unsigned long idle_skip_interval;
	u64 bd_wb_limit;
	u64 last_monitored_bd_reads;
	u64 last_monitored_bd_writes;
	struct shrinker *zram_shrinker;
	struct list_lru zram_list_lru;
	struct work_struct shrink_work;
	struct zram_pp_ctl *shrink_ctl;
	struct work_struct gc_work;
	struct delayed_work gc_periodic_work;
	struct list_head shadow_caches;
	struct list_head shadow_prefetches;
	spinlock_t wb_limit_lock;
	spinlock_t bitmap_lock;
	spinlock_t shadow_lock;
	atomic_t shrinker_writeback_in_progress;
	atomic_t gc_pending;
	u32 gc_target_pages;
	u32 shadow_cache_bytes;
	u32 shadow_cache_limit;
	u32 shadow_cache_next_age;
	u32 shadow_hits_window;
	u32 shadow_access_window;
	bool wb_limit_enable;
	bool stop_writeback;
	bool prefetch_disabled;
	bool emergency_reclaim;
};
#endif

#ifdef CONFIG_ZRAM_MULTI_COMP
#define ZRAM_PRIMARY_COMP	0U
#define ZRAM_SECONDARY_COMP	1U
#define ZRAM_MAX_COMPS	4U
#else
#define ZRAM_PRIMARY_COMP	0U
#define ZRAM_SECONDARY_COMP	0U
#define ZRAM_MAX_COMPS	1U
#endif

struct zram {
	struct zram_table_entry *table;
	struct zs_pool *mem_pool;
	struct zcomp *comps[ZRAM_MAX_COMPS];
	struct gendisk *disk;
	/* Prevent concurrent execution of device init */
	struct rw_semaphore init_lock;
	/*
	 * the number of pages zram can consume for storing compressed data
	 */
	unsigned long limit_pages;

	struct zram_stats stats;
	/*
	 * This is the limit on amount of *uncompressed* worth of data
	 * we can store in a disk.
	 */
	u64 disksize;	/* bytes */
	const char *comp_algs[ZRAM_MAX_COMPS];
#ifdef CONFIG_ZRAM_WRITEBACK
	struct zram_wb_state *wb;
#endif
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
	struct dentry *debugfs_dir;
#endif
	s8 num_active_comps;
	/*
	 * zram is claimed so open request will be failed
	 */
	bool claim; /* Protected by disk->open_mutex */
	atomic_t pp_in_progress;
#ifdef CONFIG_ZRAM_AUTO_SIZE
	unsigned int historical_mem_pressure;
	unsigned int historical_zram_pressure;
	spinlock_t pressure_lock; 
	u64 historical_disksize;
#endif
};

struct zram_opt_stats {
	u64 pages_stored;
	u64 huge_pages;
	u64 compr_data_size;
	u64 writestall;
	u64 bd_reads;
	u64 bd_writes;
	unsigned long disksize_pages;
	unsigned long limit_pages;
	bool has_writeback;
	bool has_capacity;
};

int zram_memcg_init(void);
void zram_memcg_exit(void);
bool zram_get_opt_stats(struct zram_opt_stats *stats);
int zram_opt_init(void);
void zram_opt_exit(void);

void zram_slot_lock(struct zram *zram, u32 index);
void zram_slot_unlock(struct zram *zram, u32 index);
void zram_set_handle(struct zram *zram, u32 index, unsigned long handle);
bool zram_test_flag(struct zram *zram, u32 index, enum zram_pageflags flag);
void zram_set_flag(struct zram *zram, u32 index, enum zram_pageflags flag);
void zram_free_page(struct zram *zram, size_t index);

#ifdef CONFIG_ZRAM_WRITEBACK
unsigned long zram_get_wb_blk_idx(struct zram *zram, u32 index);
unsigned long zram_get_wb_cluster_base(struct zram *zram, u32 index);
u32 zram_get_wb_cluster_off(struct zram *zram, u32 index);
void zram_set_wb_handle(struct zram *zram, u32 index,
			unsigned long cluster_base, u32 cluster_off,
			u8 nr_pages);
int zram_read_wb_page_sync(struct zram *zram, struct page *page,
			   unsigned long entry);
#endif

#if defined CONFIG_ZRAM_WRITEBACK || defined CONFIG_ZRAM_MULTI_COMP
struct zram_pp_memcg_group;

struct zram_pp_slot {
	unsigned long		index;
	u16			memcg_id;
	u16			bucket_id;
	struct zram_pp_memcg_group *group;
	struct list_head	entry;
	struct list_head	group_entry;
};

struct zram_pp_memcg_group {
	u16			memcg_id;
	unsigned int		count;
	struct list_head	node;
	struct list_head	slots;
};

/*
 * A post-processing bucket is, essentially, a size class, this defines
 * the range (in bytes) of pp-slots sizes in particular bucket.
 */
#define PP_BUCKET_SIZE_RANGE	64
#define NUM_PP_BUCKETS		((PAGE_SIZE / PP_BUCKET_SIZE_RANGE) + 1)

struct zram_pp_ctl {
	struct list_head	pp_buckets[NUM_PP_BUCKETS];
	struct list_head	pp_groups[NUM_PP_BUCKETS];
	struct completion	all_done;
	atomic_t		num_pp_slots;
};

void free_pp_slot(struct zram *zram, struct zram_pp_slot *pps);
#endif

#endif

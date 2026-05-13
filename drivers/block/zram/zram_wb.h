/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZRAM_WRITEBACK_H_
#define _ZRAM_WRITEBACK_H_

#include <linux/bio.h>
#include "zram_drv.h"

/* 定义最大合并数量 */
#define ZRAM_WB_MAX_BATCH_SIZE 64

/* 单个页面请求的元数据 */
struct zram_wb_sub_req {
	struct zram_pp_slot *pps;
	unsigned long blk_idx;  /* 物理块索引 */
	unsigned long index;    /* ZRAM 逻辑索引 (table index) */
	u8 cluster_off;
};

/* 
 * 批次请求结构
 * 这个结构体将驻留在 bio 的 front_pad 区域中
 */
struct zram_wb_batch_request {
	struct zram *zram;
	struct zram_pp_ctl *ppctl;
	struct bio *bio;
	struct list_head node;
	unsigned long start_blk_idx;
	
	/* 当前批次中包含的有效子请求数量 */
	unsigned int count;
	
	/* 记录每个页面的元数据，用于回调时释放资源 */
	struct zram_wb_sub_req sub_reqs[ZRAM_WB_MAX_BATCH_SIZE];
};

struct zram_wb_request_list {
	struct list_head head;
	int count;
	spinlock_t lock;
};

struct zram_wb_frag_score {
	unsigned long cluster_base;
	unsigned int used;
	unsigned int holes;
	unsigned int max_run;
	unsigned int transitions;
	unsigned int score;
};

struct zram_wb_memcg_group {
	u16 memcg_id;
	u16 count;
};

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct page *page;
	int error;
};

struct zram_wb_read_request {
	struct completion done;
	struct zram *zram;
	struct bio *bio;
	struct page **pages;
	unsigned int count;
	int error;
};

struct zram_shrink_work {
	struct zram *zram;
	unsigned long candidates[BATCH_SIZE];
	struct zram_pp_ctl *ctl;
	int nr_candidates;
};

#if IS_ENABLED(CONFIG_ZRAM_WRITEBACK)
unsigned long alloc_block_bdev(struct zram *zram);
unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count);
void free_block_bdev(struct zram *zram, unsigned long blk_idx);
void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count);
int zram_gc_compact(struct zram *zram, int target_pages);
void zram_schedule_gc(struct zram *zram, int target_pages);
void zram_cancel_gc(struct zram *zram);
void zram_init_gc(struct zram *zram);

struct zram_wb_batch_request *alloc_wb_batch_request(struct zram *zram,
						     struct zram_pp_ctl *ctl,
						     unsigned long start_blk_idx);
int zram_read_wb_pages_sync(struct zram *zram, struct page **pages,
				  unsigned long start_entry, unsigned int nr_pages);

int setup_zram_writeback(void);
void destroy_zram_writeback(void);
#else
inline unsigned long alloc_block_bdev(struct zram *zram) { return 0; }
inline unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count) { return 0; }
inline void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
inline void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count) {};
inline int setup_zram_writeback(void) { return 0; }
inline void destroy_zram_writeback(void) {}
#endif

#endif /* _ZRAM_WRITEBACK_H_ */

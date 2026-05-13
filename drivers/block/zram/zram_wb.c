// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_wb"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>
#include <linux/wait_bit.h>

#include "zram_wb.h"

static struct task_struct *wb_thread;
static DECLARE_WAIT_QUEUE_HEAD(wb_wq);
static struct zram_wb_request_list wb_req_list;
static struct bio_set zram_wb_bs;

static bool zram_wb_allowed(struct zram *zram);

static void zram_gc_workfn(struct work_struct *work)
{
	struct zram_wb_state *wb = container_of(work, struct zram_wb_state,
					       gc_work);
	struct zram *zram = wb->owner;
	unsigned int target_pages = READ_ONCE(zram->wb->gc_target_pages);

	if (target_pages > ZRAM_WB_MAX_BATCH_SIZE)
		target_pages = ZRAM_WB_MAX_BATCH_SIZE;
	if (target_pages < 2)
		target_pages = 2;

	if (down_read_trylock(&zram->init_lock)) {
		if (zram->disksize && zram->wb->backing_dev && zram_wb_allowed(zram))
			zram_gc_compact(zram, target_pages);
		up_read(&zram->init_lock);
	}

	atomic_set(&zram->wb->gc_pending, 0);
}

static void zram_gc_periodic_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct zram_wb_state *wb = container_of(dwork, struct zram_wb_state,
					       gc_periodic_work);
	struct zram *zram = wb->owner;

	if (down_read_trylock(&zram->init_lock)) {
		if (zram->disksize && zram->wb->backing_dev && zram_wb_allowed(zram) &&
		    !READ_ONCE(zram->wb->prefetch_disabled) &&
		    !READ_ONCE(zram->wb->emergency_reclaim))
			zram_gc_compact(zram, ZRAM_GC_PERIODIC_PAGES);
		up_read(&zram->init_lock);
	}

	queue_delayed_work(system_unbound_wq, &zram->wb->gc_periodic_work,
				 ZRAM_GC_PERIODIC_INTERVAL);
}

static bool zram_wb_allowed(struct zram *zram)
{
	return !READ_ONCE(zram->wb->stop_writeback);
}

static void zram_wb_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static void zram_wb_clear_migration(struct zram *zram, u32 index)
{
	zram_wb_clear_flag(zram, index, ZRAM_STATE_MIGRATING);
	smp_mb();
	wake_up_bit(&zram->table[index].flags, ZRAM_STATE_MIGRATING);
}

static unsigned long zram_wb_cluster_max_free_run(struct zram *zram,
		unsigned long cluster_base)
{
	unsigned long cluster_end = min(cluster_base + ZRAM_WB_CLUSTER_SIZE,
					 zram->wb->nr_pages);
	unsigned long max_free_run = 0;
	unsigned long current_free_run = 0;
	unsigned long blk;

	for (blk = cluster_base; blk < cluster_end; blk++) {
		if (test_bit(blk, zram->wb->bitmap)) {
			max_free_run = max(max_free_run, current_free_run);
			current_free_run = 0;
		} else {
			current_free_run++;
		}
	}

	return max(max_free_run, current_free_run);
}

static void zram_replace_block_bdev(struct zram *zram,
		unsigned long old_blk_idx, unsigned long new_blk_idx)
{
	unsigned long flags;

	if (old_blk_idx == new_blk_idx)
		return;

	spin_lock_irqsave(&zram->wb->bitmap_lock, flags);
	if (WARN_ON_ONCE(!test_bit(new_blk_idx, zram->wb->bitmap)))
		set_bit(new_blk_idx, zram->wb->bitmap);
	if (!test_and_clear_bit(old_blk_idx, zram->wb->bitmap))
		WARN_ON_ONCE(1);
	spin_unlock_irqrestore(&zram->wb->bitmap_lock, flags);
}

static bool zram_wb_slot_can_gc(struct zram *zram, u32 index)
{
	return (zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1) ||
		zram_test_flag(zram, index, ZRAM_SAME) ||
		zram_test_flag(zram, index, ZRAM_WB)) &&
	       zram_test_flag(zram, index, ZRAM_WB) &&
	       !zram_test_flag(zram, index, ZRAM_PP_SLOT) &&
	       zram->table[index].migration_count < 3;
}

static u16 zram_wb_preferred_memcg(struct zram *zram,
		unsigned long cluster_base)
{
	struct zram_wb_memcg_group groups[ZRAM_WB_CLUSTER_SIZE];
	unsigned long cluster_end = min(cluster_base + ZRAM_WB_CLUSTER_SIZE,
					 zram->wb->nr_pages);
	u16 best_id = 0;
	unsigned int best_count = 0;
	int nr_groups = 0;
	unsigned long i;

	memset(groups, 0, sizeof(groups));

	for (i = 0; i < cluster_end - cluster_base; i++) {
		u32 index = cluster_base + i;
		u16 memcg_id;
		int g;

		zram_slot_lock(zram, index);
		if (!zram_wb_slot_can_gc(zram, index)) {
			zram_slot_unlock(zram, index);
			continue;
		}
		memcg_id = zram->table[index].memcg_id;
		zram_slot_unlock(zram, index);

		for (g = 0; g < nr_groups; g++) {
			if (groups[g].memcg_id == memcg_id) {
				groups[g].count++;
				goto next;
			}
		}

		groups[nr_groups].memcg_id = memcg_id;
		groups[nr_groups].count = 1;
		nr_groups++;
next:
		if (groups[nr_groups ? nr_groups - 1 : 0].count > best_count) {
			best_count = groups[nr_groups ? nr_groups - 1 : 0].count;
			best_id = groups[nr_groups ? nr_groups - 1 : 0].memcg_id;
		}
	}

	for (i = 0; i < nr_groups; i++) {
		if (groups[i].count > best_count) {
			best_count = groups[i].count;
			best_id = groups[i].memcg_id;
		}
	}

	return best_id;
}

static int zram_wb_collect_memcg(struct zram *zram, u16 memcg_id,
		unsigned long *indexes, unsigned long *old_blks,
		int nr_selected, int target_pages)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long i;

	for (i = 0; i < nr_pages && nr_selected < target_pages; i++) {
		unsigned long blk_idx;

		zram_slot_lock(zram, i);
		if (!zram_wb_slot_can_gc(zram, i) ||
		    zram->table[i].memcg_id != memcg_id) {
			zram_slot_unlock(zram, i);
			continue;
		}

		blk_idx = zram_get_wb_blk_idx(zram, i);
		zram_set_flag(zram, i, ZRAM_PP_SLOT);
		zram_set_flag(zram, i, ZRAM_STATE_MIGRATING);
		indexes[nr_selected] = i;
		old_blks[nr_selected] = blk_idx;
		nr_selected++;
		zram_slot_unlock(zram, i);
	}

	return nr_selected;
}

static int zram_wb_count_groups(struct zram *zram,
		struct zram_wb_memcg_group *groups, int max_groups)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	int nr_groups = 0;
	unsigned long i;

	memset(groups, 0, sizeof(*groups) * max_groups);

	for (i = 0; i < nr_pages; i++) {
		u16 memcg_id;
		int g;

		zram_slot_lock(zram, i);
		if (!zram_wb_slot_can_gc(zram, i)) {
			zram_slot_unlock(zram, i);
			continue;
		}
		memcg_id = zram->table[i].memcg_id;
		zram_slot_unlock(zram, i);

		for (g = 0; g < nr_groups; g++) {
			if (groups[g].memcg_id == memcg_id) {
				groups[g].count++;
				goto next;
			}
		}

		if (nr_groups >= max_groups)
			continue;
		groups[nr_groups].memcg_id = memcg_id;
		groups[nr_groups].count = 1;
		nr_groups++;
next:
		continue;
	}

	return nr_groups;
}

static unsigned int zram_wb_cluster_score(struct zram *zram,
		unsigned long cluster_base,
		struct zram_wb_frag_score *frag)
{
	unsigned long cluster_end = min(cluster_base + ZRAM_WB_CLUSTER_SIZE,
					 zram->wb->nr_pages);
	unsigned int used = 0;
	unsigned int dominant_memcg = 0;
	unsigned int holes = 0;
	unsigned int max_run = 0;
	unsigned int transitions = 0;
	unsigned int current_run = 0;
	unsigned int memcg_bonus = 0;
	bool prev_set = false;
	bool seen_prev = false;
	unsigned long blk;

	for (blk = cluster_base; blk < cluster_end; blk++) {
		bool set = test_bit(blk, zram->wb->bitmap);

		if (set) {
			used++;
			current_run++;
			max_run = max(max_run, current_run);
		} else {
			if (current_run)
				holes++;
			current_run = 0;
		}

		if (seen_prev && prev_set != set)
			transitions++;
		prev_set = set;
		seen_prev = true;
	}

	if (used < 2 || used >= (cluster_end - cluster_base) || max_run == used)
		return 0;

	dominant_memcg = 0;
	if (used)
		dominant_memcg = zram_wb_preferred_memcg(zram, cluster_base);
	if (dominant_memcg) {
		unsigned long idx;

		for (idx = cluster_base; idx < cluster_end; idx++) {
			u16 memcg_id;

			zram_slot_lock(zram, idx);
			if (!zram_wb_slot_can_gc(zram, idx)) {
				zram_slot_unlock(zram, idx);
				continue;
			}
			memcg_id = zram->table[idx].memcg_id;
			zram_slot_unlock(zram, idx);
			if (memcg_id == dominant_memcg)
				memcg_bonus++;
		}
	}

	frag->cluster_base = cluster_base;
	frag->used = used;
	frag->holes = holes;
	frag->max_run = max_run;
	frag->transitions = transitions;
	frag->score = (used - max_run) * 4 + holes * 2 + transitions +
			  memcg_bonus * 2 + min_t(unsigned long,
				  zram->wb->shadow_last_hit_rate / 10, 10UL);

	return frag->score;
}

static bool zram_wb_find_best_cluster(struct zram *zram,
		struct zram_wb_frag_score *best)
{
	unsigned long cluster_base;
	bool found = false;

	memset(best, 0, sizeof(*best));

	for (cluster_base = 0; cluster_base < zram->wb->nr_pages;
	     cluster_base += ZRAM_WB_CLUSTER_SIZE) {
		struct zram_wb_frag_score cur;

		if (!zram_wb_cluster_score(zram, cluster_base, &cur))
			continue;

		if (!found || cur.score > best->score ||
		    (cur.score == best->score && cur.used > best->used)) {
			*best = cur;
			found = true;
		}
	}

	return found;
}

static int zram_wb_write_page(struct zram *zram, struct page *page,
			      unsigned long blk_idx)
{
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, zram->wb->bdev, &bv, 1, REQ_OP_WRITE);
	bio.bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
	__bio_add_page(&bio, page, PAGE_SIZE, 0);

	return submit_bio_wait(&bio);
}

int zram_gc_compact(struct zram *zram, int target_pages)
{
	unsigned long indexes[ZRAM_WB_MAX_BATCH_SIZE];
	unsigned long old_blks[ZRAM_WB_MAX_BATCH_SIZE];
	struct page *pages[ZRAM_WB_MAX_BATCH_SIZE];
	struct zram_wb_memcg_group *groups;
	struct zram_wb_frag_score best;
	u16 preferred_memcg;
	unsigned long new_base;
	int act_count = 0;
	int nr_selected = 0;
	int nr_groups;
	int i;
	bool force_reclaim = false;

	if (!zram_wb_allowed(zram) || target_pages <= 1 ||
	    target_pages > ZRAM_WB_MAX_BATCH_SIZE)
		return 0;

	for (i = 0; i < target_pages; i++)
		pages[i] = NULL;

	groups = kcalloc(ZRAM_WB_CLUSTER_SIZE, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return 0;

	if (!zram_wb_find_best_cluster(zram, &best))
		goto free_groups;
	preferred_memcg = zram_wb_preferred_memcg(zram, best.cluster_base);

	if (preferred_memcg)
		nr_selected = zram_wb_collect_memcg(zram, preferred_memcg,
						   indexes, old_blks,
						   nr_selected, target_pages);

	nr_groups = zram_wb_count_groups(zram, groups, ZRAM_WB_CLUSTER_SIZE);
	while (nr_selected < target_pages) {
		int remaining = target_pages - nr_selected;
		u16 picked_memcg = 0;
		unsigned int picked_count = 0;
		bool whole_fit = false;
		int g;

		for (g = 0; g < nr_groups; g++) {
			if (!groups[g].count || groups[g].memcg_id == preferred_memcg)
				continue;

			if (groups[g].count <= remaining) {
				if (!whole_fit || groups[g].count > picked_count) {
					picked_memcg = groups[g].memcg_id;
					picked_count = groups[g].count;
					whole_fit = true;
				}
			} else if (!whole_fit && groups[g].count > picked_count) {
				picked_memcg = groups[g].memcg_id;
				picked_count = groups[g].count;
			}
		}

		if (!picked_memcg)
			break;

		nr_selected = zram_wb_collect_memcg(zram, picked_memcg,
						   indexes, old_blks,
						   nr_selected, target_pages);
		for (g = 0; g < nr_groups; g++) {
			if (groups[g].memcg_id == picked_memcg)
				groups[g].count = 0;
		}
	}

	if (nr_selected < 2)
		goto rollback;

	new_base = alloc_block_bdev_batch(zram, nr_selected, &act_count);
	if (!new_base || act_count < nr_selected) {
		if (new_base)
			free_block_bdev_range(zram, new_base, act_count);
		goto rollback;
	}

	for (i = 0; i < nr_selected; i++) {
		pages[i] = alloc_page(GFP_NOIO | __GFP_NOWARN);
		if (!pages[i])
			goto free_new_range;

		if (zram_read_wb_page_sync(zram, pages[i], old_blks[i]))
			goto free_new_range;
	}

	for (i = 0; i < nr_selected; i++) {
		if (zram_wb_write_page(zram, pages[i], new_base + i))
			goto free_new_range;
	}

	for (i = 0; i < nr_selected; i++) {
		unsigned long index = indexes[i];

		zram_slot_lock(zram, index);
		if (!zram_wb_slot_can_gc(zram, index) ||
		    zram_get_wb_blk_idx(zram, index) != old_blks[i]) {
			free_block_bdev(zram, new_base + i);
			zram_wb_clear_migration(zram, index);
			zram_wb_clear_flag(zram, index, ZRAM_PP_SLOT);
			zram_slot_unlock(zram, index);
			continue;
		}

		zram_set_wb_handle(zram, index, new_base, i, nr_selected);
		if (zram->table[index].migration_count < 3)
			zram->table[index].migration_count++;
		zram_wb_clear_migration(zram, index);
		zram_wb_clear_flag(zram, index, ZRAM_PP_SLOT);
		zram_slot_unlock(zram, index);
		zram_replace_block_bdev(zram, old_blks[i], new_base + i);
	}

	if (zram_wb_cluster_max_free_run(zram, best.cluster_base) <
	    zram->wb->reclaim_threshold)
		force_reclaim = true;

	for (i = 0; i < nr_selected; i++) {
		__free_page(pages[i]);
		pages[i] = NULL;
	}

	if (force_reclaim && zram_wb_allowed(zram))
		zram_wb_find_best_cluster(zram, &best);

	kfree(groups);

	return nr_selected;

free_new_range:
	free_block_bdev_range(zram, new_base, nr_selected);
	for (i = 0; i < nr_selected; i++) {
		if (pages[i]) {
			__free_page(pages[i]);
			pages[i] = NULL;
		}
	}

rollback:
	for (i = 0; i < nr_selected; i++) {
		zram_slot_lock(zram, indexes[i]);
		if (zram_test_flag(zram, indexes[i], ZRAM_STATE_MIGRATING))
			zram_wb_clear_migration(zram, indexes[i]);
		if (zram_test_flag(zram, indexes[i], ZRAM_PP_SLOT))
			zram_wb_clear_flag(zram, indexes[i], ZRAM_PP_SLOT);
		zram_slot_unlock(zram, indexes[i]);
	}

free_groups:
	kfree(groups);

	return 0;
}

void zram_schedule_gc(struct zram *zram, int target_pages)
{
	if (!zram_wb_allowed(zram))
		return;

	if (target_pages > ZRAM_WB_MAX_BATCH_SIZE)
		target_pages = ZRAM_WB_MAX_BATCH_SIZE;
	if (target_pages < 2)
		target_pages = 2;

	WRITE_ONCE(zram->wb->gc_target_pages, target_pages);
	if (atomic_cmpxchg(&zram->wb->gc_pending, 0, 1) == 0)
		queue_work(system_unbound_wq, &zram->wb->gc_work);
}

void zram_cancel_gc(struct zram *zram)
{
	cancel_work_sync(&zram->wb->gc_work);
	cancel_delayed_work_sync(&zram->wb->gc_periodic_work);
	atomic_set(&zram->wb->gc_pending, 0);
}

void zram_init_gc(struct zram *zram)
{
	INIT_WORK(&zram->wb->gc_work, zram_gc_workfn);
	INIT_DELAYED_WORK(&zram->wb->gc_periodic_work, zram_gc_periodic_workfn);
	atomic_set(&zram->wb->gc_pending, 0);
	zram->wb->gc_target_pages = ZRAM_WB_MAX_BATCH_SIZE;
}

/* 
 * front_pad: 在 bio 结构之前预留空间存放 zram_wb_batch_request
 * 这个结构现在比较大 (包含数组)，必须确保 bio 对齐
 */
#define ZRAM_WB_FRONT_PAD_WRITE \
	roundup(sizeof(struct zram_wb_batch_request), __alignof__(struct bio))

#define ZRAM_WB_FRONT_PAD_READ \
	roundup(sizeof(struct zram_wb_read_request), __alignof__(struct bio))

#define ZRAM_WB_FRONT_PAD \
	max(ZRAM_WB_FRONT_PAD_WRITE, ZRAM_WB_FRONT_PAD_READ)

/*
 * 从 bio 指针获取其前面的 zram_wb_batch_request 结构
 */
#define bio_to_wb_batch(bio) \
	((struct zram_wb_batch_request *)((char *)(bio) - ZRAM_WB_FRONT_PAD))

#define bio_to_wb_read_req(bio) \
	((struct zram_wb_read_request *)((char *)(bio) - ZRAM_WB_FRONT_PAD))

/* 
 * 内部辅助函数：尝试分配指定长度的连续区间
 * 返回值：成功返回起始索引，失败返回 0
 */
static unsigned long alloc_block_bdev_range(struct zram *zram, int count)
{
	unsigned long blk_idx = 1; /* skip 0 bit */
	unsigned long flags;
	/*
	 * 自动对齐：尝试让起始索引按 count 对齐 (前提 count 是 2 的幂)
     * 这样可以显著提高底层块设备的合并效率
	 */
    unsigned long align_mask = (unsigned long)count - 1;

	spin_lock_irqsave(&zram->wb->bitmap_lock, flags);
	if (zram->wb->dirty_free_bitmap) {
		blk_idx = bitmap_find_next_zero_area(zram->wb->bitmap, zram->wb->nr_pages,
					     blk_idx, count, align_mask);
		while (blk_idx < zram->wb->nr_pages) {
			if (bitmap_weight(zram->wb->dirty_free_bitmap + BIT_WORD(blk_idx),
					 min_t(unsigned long, count,
					       zram->wb->nr_pages - blk_idx)) == count)
				break;
			blk_idx = bitmap_find_next_zero_area(zram->wb->bitmap, zram->wb->nr_pages,
					     blk_idx + 1, count, align_mask);
		}
	}
	if (blk_idx >= zram->wb->nr_pages)
	blk_idx = bitmap_find_next_zero_area(zram->wb->bitmap, zram->wb->nr_pages,
					     blk_idx, count, align_mask);
	if (blk_idx < zram->wb->nr_pages) {
		bitmap_set(zram->wb->bitmap, blk_idx, count);
		if (zram->wb->dirty_free_bitmap)
			bitmap_clear(zram->wb->dirty_free_bitmap, blk_idx, count);
	}
	spin_unlock_irqrestore(&zram->wb->bitmap_lock, flags);

	if (blk_idx >= zram->wb->nr_pages)
		return 0;

	/* 成功分配 */
	percpu_counter_add(&zram->stats.bd_count, count);
	return blk_idx;
}

/*
 * 实现 TODO 1.2: 分配降级策略
 * req_count: 请求分配的块数 (例如 64)
 * act_count: 输出参数，实际分配的块数
 */
unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count)
{
    static const int FALLBACK_SIZES[] = {64, 32, 16, 8, 4, 2, 1};
    int i;

    for (i = 0; i < ARRAY_SIZE(FALLBACK_SIZES); i++) {
        int try_count = FALLBACK_SIZES[i];
        if (try_count > req_count) continue;   // 不超过请求量

        unsigned long blk_idx = alloc_block_bdev_range(zram, try_count);
        if (blk_idx) {
            if (act_count) *act_count = try_count;
            return blk_idx;
        }
    }

    if (act_count) *act_count = 0;
    return 0;
}

/* 保持原有单块分配函数的兼容性 */
unsigned long alloc_block_bdev(struct zram *zram)
{
	int count = 0;
	return alloc_block_bdev_batch(zram, 1, &count);
}

/* 新增：批量释放辅助函数 */
void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count)
{
	int i;
	unsigned long flags;
	/* 
	 * 注意：这里假设调用者保证了范围的合法性
	 * 逐个清除位
	 */
	spin_lock_irqsave(&zram->wb->bitmap_lock, flags);
	for (i = 0; i < count; i++) {
		if (!test_and_clear_bit(blk_idx + i, zram->wb->bitmap))
			WARN_ON_ONCE(1); /* 释放了未分配的块 */
	}
	spin_unlock_irqrestore(&zram->wb->bitmap_lock, flags);
	percpu_counter_sub(&zram->stats.bd_count, count);
}

/* 保持原有单块释放函数的兼容性 */
void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	free_block_bdev_range(zram, blk_idx, 1);
}


/*
 * 处理完成的 BIO 批次
 * 这是一个核心函数，负责批量释放资源
 */
static void complete_wb_batch(struct zram_wb_batch_request *req)
{
	struct zram *zram = req->zram;
	struct zram_pp_ctl *ctl = req->ppctl;
	struct bio *bio = req->bio;
	bool io_error = bio->bi_status != BLK_STS_OK;
	int i;

	/* 遍历批次中的每一个子请求 */
	for (i = 0; i < req->count; i++) {
		struct zram_wb_sub_req *sub = &req->sub_reqs[i];
		unsigned long index = sub->index;
		unsigned long blk_idx = sub->blk_idx;
		struct zram_pp_slot *pps = sub->pps;

		if (io_error)
			goto handle_err;

		/* 更新统计 */
		percpu_counter_inc(&zram->stats.bd_writes);

		/* 锁定槽位进行状态变更 */
		zram_slot_lock(zram, index);

		/* 
		 * 极少数情况：在写回期间槽位被重新分配或释放了
		 * 我们必须检查 ZRAM_PP_SLOT 标志
		 */
		if (!zram_test_flag(zram, index, ZRAM_PP_SLOT)) {
			zram_slot_unlock(zram, index);
			goto handle_err;
		}

		/* 成功路径：释放内存页，设置写回标志 */
		zram_free_page(zram, index);
		zram_set_flag(zram, index, ZRAM_WB);
		zram_set_wb_handle(zram, index, req->start_blk_idx,
				   sub->cluster_off, req->count);
		zram->table[index].migration_count = 0;
		percpu_counter_inc(&zram->stats.pages_stored);

		/* 更新写回限制配额 */
		spin_lock(&zram->wb->wb_limit_lock);
		if (zram->wb->wb_limit_enable && zram->wb->bd_wb_limit > 0)
			zram->wb->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
		spin_unlock(&zram->wb->wb_limit_lock);

		zram_slot_unlock(zram, index);
		
		/* 释放后处理槽位包装器 */
		free_pp_slot(zram, pps);
		continue;

handle_err:
		/* 失败路径：回滚块分配，保留 ZRAM 内存页 */
		free_block_bdev(zram, blk_idx);
		free_pp_slot(zram, pps);
	}

	/* 
	 * 重要：ctl->num_pp_slots 记录了待处理的总数
	 * 此时减少当前批次处理的数量 (req->count)
	 */
	if (atomic_sub_and_test(req->count, &ctl->num_pp_slots))
		complete(&ctl->all_done);

	/* 释放 BIO 及其挂载的所有 pages */
	{
		struct bio_vec *bv;
		struct bvec_iter_all iter;
		
		bio_for_each_segment_all(bv, bio, iter) {
			__free_page(bv->bv_page);
		}
		/* bio_put 会释放 bio 内存以及 front_pad */
		bio_put(bio);
	}
}

static void enqueue_wb_request(struct zram_wb_request_list *req_list,
			       struct zram_wb_batch_request *req)
{
	spin_lock_bh(&req_list->lock);
	list_add_tail(&req->node, &req_list->head);
	req_list->count++;
	spin_unlock_bh(&req_list->lock);
}

static struct zram_wb_batch_request *dequeue_wb_request(
	struct zram_wb_request_list *req_list)
{
	struct zram_wb_batch_request *req = NULL;

	spin_lock_bh(&req_list->lock);
	if (!list_empty(&req_list->head)) {
		req = list_first_entry(&req_list->head,
				       struct zram_wb_batch_request,
				       node);
		list_del(&req->node);
		req_list->count--;
	}
	spin_unlock_bh(&req_list->lock);

	return req;
}

static void destroy_wb_request_list(struct zram_wb_request_list *req_list)
{
	struct zram_wb_batch_request *req;

	while (!list_empty(&req_list->head)) {
		req = dequeue_wb_request(req_list);
		int i;
		for(i = 0; i < req->count; i++) {
			free_block_bdev(req->zram, req->sub_reqs[i].blk_idx);
			free_pp_slot(req->zram, req->sub_reqs[i].pps);
		}
		
		/* Free pages and bio */
		struct bio_vec *bv;
		struct bvec_iter_all iter;
		bio_for_each_segment_all(bv, req->bio, iter) {
			__free_page(bv->bv_page);
		}
		bio_put(req->bio);
	}
}

static bool wb_ready_to_run(void)
{
	int count;
	spin_lock_bh(&wb_req_list.lock);
	count = wb_req_list.count;
	spin_unlock_bh(&wb_req_list.lock);
	return count > 0;
}

static int wb_thread_func(void *data)
{
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(wb_wq, wb_ready_to_run());

		while (1) {
			struct zram_wb_batch_request *req;
			req = dequeue_wb_request(&wb_req_list);
			if (!req)
				break;
			complete_wb_batch(req);
		}
	}
	return 0;
}

static void zram_writeback_end_io(struct bio *bio)
{
	struct zram_wb_batch_request *req = bio_to_wb_batch(bio);
	enqueue_wb_request(&wb_req_list, req);
	wake_up(&wb_wq);
}

static void zram_read_wb_end_io(struct bio *bio)
{
	struct zram_wb_read_request *req = bio_to_wb_read_req(bio);

	req->error = blk_status_to_errno(bio->bi_status);
	complete(&req->done);
}

int zram_read_wb_pages_sync(struct zram *zram, struct page **pages,
				  unsigned long start_entry, unsigned int nr_pages)
{
	struct zram_wb_read_request *req;
	struct bio *bio;
	unsigned int i;
	int ret;

	if (!nr_pages)
		return 0;

	bio = bio_alloc_bioset(zram->wb->bdev, nr_pages, REQ_OP_READ,
			       GFP_NOIO, &zram_wb_bs);
	if (!bio)
		return -ENOMEM;

	req = bio_to_wb_read_req(bio);
	init_completion(&req->done);
	req->zram = zram;
	req->bio = bio;
	req->pages = pages;
	req->count = nr_pages;
	req->error = 0;

	bio->bi_iter.bi_sector = start_entry * (PAGE_SIZE >> 9);
	bio->bi_end_io = zram_read_wb_end_io;

	for (i = 0; i < nr_pages; i++) {
		ret = bio_add_page(bio, pages[i], PAGE_SIZE, 0);
		if (ret != PAGE_SIZE) {
			bio_put(bio);
			return -EIO;
		}
	}

	submit_bio(bio);
	wait_for_completion(&req->done);
	ret = req->error;
	bio_put(bio);

	return ret;
}

/* 
 * 外部接口：分配一个新的批次请求 
 */
struct zram_wb_batch_request *alloc_wb_batch_request(struct zram *zram,
						     struct zram_pp_ctl *ctl,
						     unsigned long start_blk_idx)
{
	struct bio *bio;
	struct zram_wb_batch_request *req;

	/*
	 * 使用 bioset 分配 bio。
	 * ZRAM_WB_MAX_BATCH_SIZE 定义了 bio_vec 的最大数量。
	 * front_pad 会自动被 bio_alloc 分配在 bio 之前。
	 */
	bio = bio_alloc_bioset(zram->wb->bdev, ZRAM_WB_MAX_BATCH_SIZE, 
			       REQ_OP_WRITE, GFP_NOIO,
			       &zram_wb_bs);
	if (!bio)
		return NULL;

	/* 获取前面预留的结构体 */
	req = bio_to_wb_batch(bio);
	req->zram = zram;
	req->ppctl = ctl;
	req->bio = bio;
	req->count = 0; /* 初始计数为 0 */
	req->start_blk_idx = start_blk_idx;

	/* 设置 bio 的起始扇区和回调 */
	bio->bi_iter.bi_sector = start_blk_idx * (PAGE_SIZE >> 9);
	bio->bi_end_io = zram_writeback_end_io;
	
	return req;
}

int setup_zram_writeback(void)
{
	/*
	 * 初始化 bioset:
	 * pool_size: 64 (缓冲池大小)
	 * front_pad: ZRAM_WB_FRONT_PAD (包含我们的 zram_wb_batch_request)
	 */
	if (bioset_init(&zram_wb_bs, 64, ZRAM_WB_FRONT_PAD, BIOSET_NEED_BVECS)) {
		pr_err("Unable to init zram_wb_bs\n");
		return -1;
	}

	spin_lock_init(&wb_req_list.lock);
	INIT_LIST_HEAD(&wb_req_list.head);
	wb_req_list.count = 0;

	wb_thread = kthread_run(wb_thread_func, NULL, "zram_wb_thread");
	if (IS_ERR(wb_thread)) {
		pr_err("Unable to create zram_wb_thread\n");
		bioset_exit(&zram_wb_bs); 
		return -1;
	}
	return 0;
}

void destroy_zram_writeback(void)
{
	if (wb_thread)
		kthread_stop(wb_thread);
	destroy_wb_request_list(&wb_req_list);
	bioset_exit(&zram_wb_bs);
}

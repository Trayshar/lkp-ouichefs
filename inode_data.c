// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/err.h>
#include <linux/printk.h>

#include "ouichefs.h"
#include "bitmap.h"


/*
 * This function loads inode data from disk, allocating a new inode data entry
 * if it is new ('allocate'). If we want to write to the inode data, 'is_cow'
 * must be set. Setting both is illegal. The buffer_head that the inode data
 * resides in is returned in 'id_bh'; You have to release it once you are done.
 *
 * If the given inode does not exist in the current snapshot, -EINVAL is
 * returned.
 */
struct ouichefs_inode_data *ouichefs_get_inode_data(struct super_block *sb,
						    struct buffer_head **id_bh,
						    uint32_t ino, bool allocate,
						    bool is_cow)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_ino, *bh_idx, *bh_id;
	struct ouichefs_inode_data *inode_data = NULL;
	struct ouichefs_inode_data_index_block *ididx = NULL;
	struct ouichefs_inode *inode = NULL;
	uint32_t idx, bno;
	int ret;

	/* Open inode on disk; That is the snapshot -> inode data mapping */
	bh_ino = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(ino));
	if (unlikely(!bh_ino))
		return ERR_PTR(-EIO);
	inode = (struct ouichefs_inode *)bh_ino->b_data;
	inode += OUICHEFS_GET_INODE_SHIFT(ino);
	idx = inode->i_data[0];

	/* Check if idx is valid; Map new idx if necessary */
	if (allocate) {
		if (unlikely(idx != 0 && !is_cow))
			pr_warn("Residual idx %u in new inode %u\n", idx, ino);
		idx = get_free_id_entry(sbi);
		if (!idx) {
			brelse(bh_ino);
			return ERR_PTR(-ENOSPC);
		}
	} else if (idx == 0 || idx >= sbi->nr_inode_data_entries) {
		pr_warn("Illegal access to idx=%u (ino=%u)\n", idx, ino);
		brelse(bh_ino);
		return ERR_PTR(-EINVAL);
	}

	pr_debug("ino=%u, idx=%u, IDIDX_BLOCK=%u, IDIDX_INDEX=%lu, IDIDX_SHIFT=%lu\n",
		ino, idx, OUICHEFS_GET_IDIDX_BLOCK(sbi, idx),
		OUICHEFS_GET_IDIDX_INDEX(sbi, idx), OUICHEFS_GET_IDIDX_SHIFT(sbi, idx)
	);

	/* Open the inode data index */
	bh_idx = sb_bread(sb, OUICHEFS_GET_IDIDX_BLOCK(sbi, idx));
	if (unlikely(!bh_idx)) {
		ret = -EIO;
		goto failed_idx;
	}
	ididx = (struct ouichefs_inode_data_index_block *)bh_idx->b_data;
	bno = ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)];

	/* Check if bno is valid; Allocate new block if necessary */
	if (allocate && bno == 0) {
		ret = ouichefs_alloc_block(sb, &bno);

		if (unlikely(ret))
			goto failed_bno;
	} else if (bno < OUICHEFS_GET_DATA_START(sbi) || bno >= sbi->nr_blocks) {
		pr_warn("Illegal access to bno=%u (idx=%u, ino=%u)\n",
			 bno, idx, ino);
		ret = -EINVAL;
		goto failed_bno;
	}

	/* Open the inode data block */
	bh_id = sb_bread(sb, bno);
	if (unlikely(!bh_id)) {
		ret = -EIO;
		goto failed_alloc_bno;
	}
	inode_data = (struct ouichefs_inode_data *)bh_id->b_data;
	inode_data += OUICHEFS_GET_IDIDX_SHIFT(sbi, idx);

	/* Set reference counter if this is a new inode */
	if (allocate) {
		inode_data->refcount = 1;
		mark_buffer_dirty(bh_id);
		sync_dirty_buffer(bh_id);
	} else if (inode_data->refcount == 0)
		pr_warn("Refcount is 0! (idx=%u, ino=%u)\n", idx, ino);

	/*
	 * We wanna allocate a new inode data entry to write into if
	 * it is referenced by multiple snapshots. We do so by calling
	 * this function again with 'allocate' set. We do not have to
	 * copy any data from the old entry since it'll be overwritten
	 * directly anyway.
	 * Since 'allocate' is not set, no new blocks were allocated, so we can
	 * safely release all buffers.
	 * Note that usually, we decrement the idx refcount alongside the
	 * refcount of the index block; This must not be done here!
	 * If we have the same index block between both idx, then the count of
	 * idx that reference it stays the same. If we CoW'ed the
	 * index block, ouichefs_cow_block() already updated the refcount.
	 */
	if (is_cow && !allocate && inode_data->refcount > 1) {
		pr_debug("ino=%u, idx=%u, bno=%u, refcount=%u: CoWing it!\n",
			ino, idx, bno, inode_data->refcount);
		inode_data->refcount--;
		mark_buffer_dirty(bh_id);
		sync_dirty_buffer(bh_id);
		brelse(bh_id);
		brelse(bh_idx);
		brelse(bh_ino);
		return ouichefs_get_inode_data(sb, id_bh, ino, true, true);
	}

	pr_debug("ino=%u, idx=%u, bno=%u, refcount=%u\n",
		ino, idx, bno, inode_data->refcount);

	/* Update and release intermediate blocks */
	if (ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)] != bno) {
		pr_debug("Allocated bno=%u (idx=%u, ino=%u)\n",
			bno, idx, ino);
		ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)] = bno;
		mark_buffer_dirty(bh_idx);
	}
	brelse(bh_idx);
	if (inode->i_data[0] != idx) {
		pr_debug("Mapped idx=%u (ino=%u)\n", idx, ino);
		inode->i_data[0] = idx;
		mark_buffer_dirty(bh_ino);
	}
	brelse(bh_ino);

	/* Return bh_id (for later release) and inode data stored within */
	*id_bh = bh_id;
	return inode_data;

	/* Error handling; Discard any changed data and free allocated blocks */
failed_alloc_bno:
	if (ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)] != bno)
		ouichefs_put_block(sb, bno, OUICHEFS_DATA);
failed_bno:
	bforget(bh_idx);
failed_idx:
	if (inode->i_data[0] != idx)
		put_inode_data_entry(sbi, idx);
	bforget(bh_ino);
	return ERR_PTR(ret);
}

/*
 * Shares inode data across two snapshots
 */
int ouichefs_link_inode_data(struct super_block *sb, uint32_t ino,
			     struct ouichefs_inode *inode,
			     ouichefs_snap_index_t from,
			     ouichefs_snap_index_t to)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	struct ouichefs_inode_data_index_block *ididx = NULL;
	struct ouichefs_inode_data *inode_data = NULL;
	uint32_t idx = inode->i_data[from], bno;

	/* Check if inodes are already linked */
	if (inode->i_data[from] == inode->i_data[to])
		return 0;

	/* Check if idx is valid */
	if (unlikely(idx == 0 || idx >= sbi->nr_inode_data_entries)) {
		pr_warn("Illegal access to idx=%u (ino=%u)\n", idx, ino);
		return -EINVAL;
	}

	/* Open the inode data index */
	bh = sb_bread(sb, OUICHEFS_GET_IDIDX_BLOCK(sbi, idx));
	if (unlikely(!bh))
		return -EIO;
	ididx = (struct ouichefs_inode_data_index_block *)bh->b_data;
	bno = ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)];
	brelse(bh);

	/* Check if bno is valid */
	if (unlikely(bno < OUICHEFS_GET_DATA_START(sbi) || bno >= sbi->nr_blocks)) {
		pr_warn("Illegal access to bno=%u (idx=%u, ino=%u)\n",
			 bno, idx, ino);
		return -EINVAL;
	}

	/* Open the inode data block */
	bh = sb_bread(sb, bno);
	if (unlikely(!bh))
		return -EIO;
	inode_data = (struct ouichefs_inode_data *)bh->b_data;
	inode_data += OUICHEFS_GET_IDIDX_SHIFT(sbi, idx);

	/* Increment reference count of inode data */
	if (unlikely(inode_data->refcount == 0)) {
		pr_warn("Refcount is 0! (idx=%u, ino=%u)\n",
			 idx, ino);
		inode_data->refcount = 1;
	}
	inode_data->refcount++;

	/*
	 * Increase reference counter of inode's index block as to not clean
	 * it up early
	 */
	ouichefs_get_block(sb, inode_data->index_block);
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Replace the inode data */
	if (inode->i_data[to])
		ouichefs_put_inode_data(sb, ino, inode, to);
	inode->i_data[to] = idx;

	return 0;
}

/*
 * Unlinks an inode from its inode data in the given snapshot. If this was
 * the last reference to that data, it is freed.
 */
void ouichefs_put_inode_data(struct super_block *sb, uint32_t ino,
			     struct ouichefs_inode *inode,
			     ouichefs_snap_index_t snapshot)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_idx, *bh_bno;
	struct ouichefs_inode_data_index_block *ididx = NULL;
	struct ouichefs_inode_data *inode_data = NULL;
	uint32_t idx = inode->i_data[snapshot], bno;
	bool free_idx = false;

	/* Unlink the data from the inode, then try to reclaim blocks */
	inode->i_data[snapshot] = 0;

	/* Check if idx is valid */
	if (unlikely(idx == 0 || idx >= sbi->nr_inode_data_entries)) {
		pr_warn("Illegal access to idx=%u (ino=%u)\n", idx, ino);
		goto check_free_ino;
	}

	/* Open the inode data index */
	bh_idx = sb_bread(sb, OUICHEFS_GET_IDIDX_BLOCK(sbi, idx));
	if (unlikely(!bh_idx))
		goto check_free_ino;
	ididx = (struct ouichefs_inode_data_index_block *)bh_idx->b_data;
	bno = ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)];

	/* Check if bno is valid */
	if (unlikely(bno < OUICHEFS_GET_DATA_START(sbi) || bno >= sbi->nr_blocks)) {
		pr_warn("Illegal access to bno=%u (idx=%u, ino=%u)\n",
			 bno, idx, ino);
		goto brelse_idx;
	}

	/*
	 * Open the inode data block. Note that anything written inside that
	 * other than the refcount may be 0 already (if the inode was never
	 * written before deletion)
	 */
	bh_bno = sb_bread(sb, bno);
	if (unlikely(!bh_bno))
		goto brelse_idx;
	inode_data = (struct ouichefs_inode_data *)bh_bno->b_data;
	inode_data += OUICHEFS_GET_IDIDX_SHIFT(sbi, idx);

	/* Sanity check */
	if (unlikely(inode_data->refcount == 0)) {
		pr_warn("Refcount is 0! (idx=%u, ino=%u)\n",
			 idx, ino);
		goto brelse_bno;
	}

	/*
	 * Decrement reference counter of inode data. The reference counter of
	 * inode_data->index_block was already put in ouichefs_unlink().
	 */
	inode_data->refcount--;

	/*
	 * Check if we are the last reference to this inode data entry.
	 * If so, mark that entry's idx as unused and check if the block
	 * in which it is located hosts any other valid entries. If not,
	 * free it as well.
	 */
	if (inode_data->refcount == 0) {
		free_idx = true;

		/* Clear this inode data entry */
		memset(inode_data, 0, sizeof(struct ouichefs_inode_data));

		/* Check if the whole inode data block is empty */
		inode_data = (struct ouichefs_inode_data *)bh_bno->b_data;
		for (int i = 0; i < OUICHEFS_IDE_PER_DATA_BLOCK; i++) {
			if ((inode_data + i)->refcount > 0)
				goto dirty_bno;
		}

		/* Free block - since we zero it anyway, discard changes */
		bforget(bh_bno);
		ouichefs_put_block(sb, bno, OUICHEFS_INODE_DATA);

		/* Unmap associated block in ididx */
		pr_debug("Unmap inode data block %u\n", bno);
		ididx->blocks[OUICHEFS_GET_IDIDX_INDEX(sbi, idx)] = 0;
		mark_buffer_dirty(bh_idx);
		goto brelse_idx;
	}

dirty_bno:
	mark_buffer_dirty(bh_bno);
brelse_bno:
	brelse(bh_bno);
brelse_idx:
	brelse(bh_idx);

	/* Free idx after all buffers are released */
	if (free_idx)
		put_inode_data_entry(sbi, idx);
check_free_ino:
	/* Check if inode is no longer used by any snapshot */
	for (int i = 0; i < OUICHEFS_MAX_SNAPSHOTS; i++) {
		if (inode->i_data[i] != 0)
			return;
	}
	put_inode(sbi, ino);
	pr_debug("Freed inode %d!\n", ino);
}

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

#include "ouichefs.h"
#include "bitmap.h"

/*
 * Allocates a new, free data block. This function marks the block as used in
 * the bitmap and sets the reference counter.
 * If this function succeeds, the number of the allocated block is written into
 * bno and 0 is returned, otherwise the return value is negative.
 */
int ouichefs_alloc_block(struct super_block *sb, uint32_t *out)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct ouichefs_metadata_block *mb;
	uint32_t bno = 0;

	/* Get a new, free data block */
	bno = get_free_block(sbi);
	if (!bno)
		return -ENOSPC;

	pr_debug("Allocating block %u (meta %u)\n", bno,
		OUICHEFS_GET_META_BLOCK(bno, sbi));

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n", bno);
		return -EIO;
	}
	mb = (struct ouichefs_metadata_block *)bh->b_data;

	/* Set counter to one */
	pr_debug("Refcount of %u: %u -> %u\n", bno,
		 mb->refcount[OUICHEFS_GET_META_SHIFT(bno)], 1);
	mb->refcount[OUICHEFS_GET_META_SHIFT(bno)] = 1;
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Return new block */
	*out = bno;
	return 0;
}

/*
 * Increments the reference counter for the given, already used data block
 */
int ouichefs_get_block(struct super_block *sb, uint32_t bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct ouichefs_metadata_block *mb;

	/* Sanity check */
	if (unlikely(bno < OUICHEFS_GET_DATA_START(sbi))) {
		pr_debug("Invalid data block number: %d\n", bno);
		return -EINVAL;
	}

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n", bno);
		return -EIO;
	}
	mb = (struct ouichefs_metadata_block *)bh->b_data;

	/* Increase reference counter by one */
	pr_debug("Refcount of %u: %u -> %u\n", bno,
		 mb->refcount[OUICHEFS_GET_META_SHIFT(bno)],
		 mb->refcount[OUICHEFS_GET_META_SHIFT(bno)] + 1);
	mb->refcount[OUICHEFS_GET_META_SHIFT(bno)] += 1;
	mark_buffer_dirty(bh);
	brelse(bh);

	return 0;
}

/*
 * Helper method for implementing Copy-on-Write on data blocks. Given
 * a pointer (bno) to an already allocated data block, this function
 * reads its reference count. If it is one, nothing is done and the
 * function returns. Otherwise, a copy of the data block is made
 * and its block number is written into bno.
 * If this block is an index block (is_index_block), then the reference
 * count of all it's referenced blocks are updated as well.
 *
 * Return value: negative on error, 0 if nothing was done, 1 if a new
 * block has been allocated
 */
int ouichefs_cow_block(struct super_block *sb, uint32_t *bno,
		       enum ouichefs_datablock_type b_type)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh1 = NULL, *bh2 = NULL, *bh1meta = NULL;
	struct ouichefs_metadata_block *mb;
	struct ouichefs_file_index_block *index;
	uint32_t old_bno = *bno, new_bno;

	/* Sanity check */
	if (unlikely(old_bno < OUICHEFS_GET_DATA_START(sbi))) {
		pr_warn("Invalid data block number: %d\n", old_bno);
		return -EINVAL;
	}

	/* Open corresponding metadata block */
	bh1meta = sb_bread(sb, OUICHEFS_GET_META_BLOCK(old_bno, sbi));
	if (unlikely(!bh1meta)) {
		pr_err("Failed to open metadata block for data block %d\n", old_bno);
		return -EIO;
	}
	mb = (struct ouichefs_metadata_block *)bh1meta->b_data;

	/* Only one reference; We can modify this block; Return! */
	if (mb->refcount[OUICHEFS_GET_META_SHIFT(old_bno)] == 1) {
		pr_debug("Refcount of %u is 1: No copy needed.\n", old_bno);
		brelse(bh1meta);
		return 0;
	}

	/* We are not the sole owner of this data */
	pr_debug("Refcount of %u is %u: CoWing it!\n", old_bno,
		mb->refcount[OUICHEFS_GET_META_SHIFT(old_bno)]);
	bh1 = sb_bread(sb, old_bno);
	if (unlikely(!bh1)) {
		brelse(bh1meta);
		return -EIO;
	}
	lock_buffer(bh1);

	/*
	 * Decrement reference counter of original data
	 */
	mb->refcount[OUICHEFS_GET_META_SHIFT(old_bno)] -= 1;
	mark_buffer_dirty(bh1meta);
	brelse(bh1meta);

	/*
	 * Allocate new data block, set it's reference count to 1 and open it.
	 * This is safe now since the metadata block of old_bno is no longer
	 * locked (old_bno and new_bno might reside in the same metadata block)
	 */
	int ret = ouichefs_alloc_block(sb, &new_bno);
	if (unlikely(ret < 0)) {
		unlock_buffer(bh1);
		brelse(bh1);
		return ret;
	}
	bh2 = sb_bread(sb, new_bno);
	if (unlikely(!bh2)) {
		pr_err("Failed to open newly-allocated data block %u!\n", new_bno);
		ouichefs_put_block(sb, new_bno, OUICHEFS_DATA);
		unlock_buffer(bh1);
		brelse(bh1);
		return -EIO;
	}

	/*
	 * Copy data and release the new block; We may need to read the data,
	 * so keep the old block here for now
	 */
	memcpy(bh2->b_data, bh1->b_data, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh2);
	brelse(bh2);

	/* Handle block types */
	switch (b_type) {
	case OUICHEFS_INDEX:
		index = (struct ouichefs_file_index_block *)bh1->b_data;
		for (int i = 0; i < OUICHEFS_INDEX_BLOCK_LEN; i++) {
			if (!index->blocks[i])
				break;
			/* Safety: No metadata blocks are currently locked */
			ouichefs_get_block(sb, index->blocks[i]);
		}
		break;
	case OUICHEFS_DIR:
		pr_debug("Called with type OUICHEFS_DIR!\n");
		break;
	case OUICHEFS_DATA:
		break;
	}

	/* Finally release the old data block and point bno to the new block */
	unlock_buffer(bh1);
	brelse(bh1);
	*bno = new_bno;
	return 1;
}

/*
 * Decrements the reference counter for the given data block. If
 * this was the last reference, the data block is freed. If this block
 * is an index block, this function is called on each linked data block.
 */
void ouichefs_put_block(struct super_block *sb, uint32_t bno,
			enum ouichefs_datablock_type b_type)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL, *bh2 = NULL;
	struct ouichefs_metadata_block *mb;
	struct ouichefs_file_index_block *index;
	bool free_data = false;

	/* Sanity check */
	if (unlikely(bno < OUICHEFS_GET_DATA_START(sbi))) {
		pr_debug("Invalid data block number: %d\n", bno);
		return;
	}

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n", bno);
		return;
	}
	mb = (struct ouichefs_metadata_block *)bh->b_data;

	/* Decrease reference counter by one */
	free_data = mb->refcount[OUICHEFS_GET_META_SHIFT(bno)] <= 1;
	pr_debug("Refcount of %u: %u -> %u\n", bno,
		 mb->refcount[OUICHEFS_GET_META_SHIFT(bno)],
		 mb->refcount[OUICHEFS_GET_META_SHIFT(bno)] - 1);
	mb->refcount[OUICHEFS_GET_META_SHIFT(bno)] -= 1;
	mark_buffer_dirty(bh);
	brelse(bh);

	/* This was the last reference; Free data block */
	if (free_data) {
		bh2 = sb_bread(sb, bno);
		if (unlikely(!bh2))
			return; // Failed to open data block; Consider it "free"

		/* Handle type-specific cleanup */
		switch (b_type) {
		case OUICHEFS_INDEX:
			index = (struct ouichefs_file_index_block *)bh2->b_data;
			for (int i = 0; i < OUICHEFS_INDEX_BLOCK_LEN; i++) {
				if (!index->blocks[i])
					break;
				/* Safety: No metadata blocks are currently locked */
				ouichefs_put_block(sb, index->blocks[i],
					OUICHEFS_DATA);
			}
			break;
		case OUICHEFS_DIR:
			pr_debug("Called with type OUICHEFS_DIR!\n");
			break;
		case OUICHEFS_DATA:
			break;
		}

		/* Zero-out the block */
		memset(bh2->b_data, 0, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh2);
		brelse(bh2);
		put_block(sbi, bno);
		pr_debug("Freed block %u\n", bno);
	}
}

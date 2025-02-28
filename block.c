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
int ouichefs_alloc_block(struct super_block *sb, uint32_t *bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct ouichefs_metadata_block *mb;

	/* Get a new, free data block */
	*bno = get_free_block(sbi);
	if (!*bno)
		return -ENOSPC;

	pr_debug("Allocating block %u (meta %u)\n", *bno,
		OUICHEFS_GET_META_BLOCK(*bno, sbi));

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(*bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n", *bno);
		return -ENODEV;
	}
	mb = (struct ouichefs_metadata_block *)bh->b_data;

	/* Set counter to one */
	pr_debug("Refcount of %u: %u -> %u\n", *bno,
		 mb->refcount[OUICHEFS_GET_META_SHIFT(*bno)], 1);
	mb->refcount[OUICHEFS_GET_META_SHIFT(*bno)] = 1;
	mark_buffer_dirty(bh);
	brelse(bh);

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
		pr_warn("Invalid data block number: %d\n", bno);
		return -EINVAL;
	}

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n", bno);
		return -ENODEV;
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
 * and it's block number is written into bno.
 * If this block is an index block (is_index_block), then the reference
 * counts of all it's referenced blocks are updated as well.
 *
 * Return value: negative on error, 0 if nothing was done, 1 if a new
 * block has been allocated
 */
int ouichefs_cow_block(struct super_block *sb, uint32_t *bno,
		       bool is_index_block)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL, *bh2 = NULL;
	struct ouichefs_metadata_block *mb;
	uint32_t original_bno;
	int ret = 0;

	/* Sanity check */
	if (unlikely(*bno < OUICHEFS_GET_DATA_START(sbi))) {
		pr_warn("Invalid data block number: %d\n", *bno);
		return -EINVAL;
	}

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(*bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n", *bno);
		return -ENODEV;
	}
	mb = (struct ouichefs_metadata_block *)bh->b_data;

	/* Only one reference; We can modify this block; Return! */
	if (mb->refcount[OUICHEFS_GET_META_SHIFT(*bno)] == 1)
		goto clean_bh;

	/* We are not the sole owner; Copy it! */
	original_bno = *bno;
	ret = ouichefs_alloc_block(sb, bno);
	if (!ret)
		goto clean_bh;

	/* Decrement reference counter of original data */
	pr_debug("Refcount of %u: %u -> %u\n", original_bno,
		 mb->refcount[OUICHEFS_GET_META_SHIFT(original_bno)],
		 mb->refcount[OUICHEFS_GET_META_SHIFT(original_bno)] - 1);
	mb->refcount[OUICHEFS_GET_META_SHIFT(original_bno)] -= 1;
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Open blocks, copy data */
	bh = sb_bread(sb, original_bno);
	if (unlikely(!bh)) {
		ret = -EIO;
		goto clean_bh;
	}
	bh2 = sb_bread(sb, *bno);
	if (unlikely(!bh2)) {
		ret = -EIO;
		goto clean_bh2;
	}
	memcpy(bh2->b_data, bh->b_data, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh2);

	/* If index block, update reference counters of all referenced blocks */
	if (is_index_block) {
		brelse(bh2);

		struct ouichefs_file_index_block *index =
			(struct ouichefs_file_index_block *)bh->b_data;
		for (int i = 0; index->blocks[i] != 0; i++) {
			/* Skip empty blocks and ignore any errors */
			if (!index->blocks[i])
				continue;
			ouichefs_get_block(sb, index->blocks[i]);
		}
		ret = 1;
		goto clean_bh;
	}

clean_bh2:
	brelse(bh2);
clean_bh:
	brelse(bh);
	return ret;
}

/*
 * Decrements the reference counter for the given data block. If
 * this was the last reference, the data block is freed.
 * Note that this function treats the given block as pure data.
 * If this block is an index block, the counters
 * of any referenced blocks are NOT decremented. You HAVE to do that.
 */
void ouichefs_put_block(struct super_block *sb, uint32_t bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct ouichefs_metadata_block *mb;
	bool free_data = false;

	/* Sanity check */
	if (unlikely(bno < OUICHEFS_GET_DATA_START(sbi))) {
		pr_warn("Invalid data block number: %d\n", bno);
		return;
	}

	/* Open corresponding metadata block */
	bh = sb_bread(sb, OUICHEFS_GET_META_BLOCK(bno, sbi));
	if (unlikely(!bh)) {
		pr_err("Failed to open metadata block for data block %d\n",
		       bno);
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
		bh = NULL;
		put_block(sbi, bno);
		bh = sb_bread(sb, bno);
		if (unlikely(!bh))
			return; // Failed to open data block; Consider it "free"

		/* Zero-out the block */
		memset(bh->b_data, 0, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		brelse(bh);
		pr_debug("Freed block %u\n", bno);
	}
}

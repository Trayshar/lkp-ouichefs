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
#include <linux/mpage.h>

#include "ouichefs.h"

static int __truncate_index_block(struct super_block *sb,
	uint32_t index_block, uint32_t start);

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it. If cow is true, check if block
 * is writeable and allocate a copy if not.
 */
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, bool create, bool cow)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	uint32_t bno;
	int ret = 0;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_INDEX_BLOCK_LEN)
		return -EFBIG;

	/* Get index block for this inode; If it is shared between different
	 * inodes and we want to write, make a copy */
	if (cow) {
		ret = ouichefs_cow_block(sb, &ci->index_block, OUICHEFS_INDEX);
		if (unlikely(ret < 0))
			return ret;

		/* Update inode to point to new copy of the index block */
		if (ret > 0)
			mark_inode_dirty(inode);
	}

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (unlikely(!bh_index))
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */
	bno = index->blocks[iblock];
	if (bno == 0) {
		if (!create) {
			ret = 0;
			goto brelse_index;
		}
		ret = ouichefs_alloc_block(sb, &bno);
		if (unlikely(ret < 0))
			goto brelse_index;

		index->blocks[iblock] = bno;
		mark_buffer_dirty(bh_index);
	} else if (cow) {
		/* Check if this block is shared; Copy it if it is */
		ret = ouichefs_cow_block(sb, &bno, OUICHEFS_DATA);
		if (unlikely(ret < 0))
			goto brelse_index;

		/* Update index block to point to newly allocated copy */
		if (ret > 0) {
			index->blocks[iblock] = bno;
			mark_buffer_dirty(bh_index);
		}
	}

	pr_debug("Mapped sector %llu to block %u (cow=%i)\n",
		iblock, bno, cow);

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

static int ouichefs_file_get_block_ro(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create)
{
	return ouichefs_file_get_block(inode, iblock, bh_result, false, false);
}

static int ouichefs_file_get_block_cow(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create)
{
	return ouichefs_file_get_block(inode, iblock, bh_result, !!create, true);
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory.
 */
static void ouichefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ouichefs_file_get_block_ro);
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int ouichefs_writepage(struct page *page, struct writeback_control *wbc)
{
	// TODO: This may be overkill, since this could copy ALL shared blocks and
	// not just those that are dirty; But how would I do that?
	return block_write_full_page(page, ouichefs_file_get_block_cow, wbc);
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 */
static int ouichefs_write_begin(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct page **pagep,
				void **fsdata)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(file->f_inode->i_sb);
	int err;
	uint32_t nr_allocs = 0;

	/* Check if the write can be completed (enough space?) */
	if (pos + len > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;
	nr_allocs = max(pos + len, i_size_read(file->f_inode)) / OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > file->f_inode->i_blocks - 1)
		nr_allocs -= file->f_inode->i_blocks - 1;
	else
		nr_allocs = 0;
	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	/* prepare the write */
	err = block_write_begin(mapping, pos, len, pagep,
		ouichefs_file_get_block_cow);
	/* if this failed, reclaim newly allocated blocks */
	if (err < 0) {
		pr_err("%s:%d: newly allocated blocks reclaim not implemented yet\n",
		       __func__, __LINE__);
	}
	return err;
}

/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int ouichefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = file->f_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;

	/* Complete the write() */
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		pr_err("%s:%d: wrote less than asked... what do I do? nothing for now...\n",
		       __func__, __LINE__);
		goto end;
	}
	uint32_t nr_blocks_old = inode->i_blocks;

	/* Update inode metadata. The 1 is the index block */
	inode->i_blocks = 1 + (i_size_read(inode) / OUICHEFS_BLOCK_SIZE);
	if ((i_size_read(inode) % OUICHEFS_BLOCK_SIZE) != 0)
		inode->i_blocks++;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	mark_inode_dirty(inode);

	/* If file is smaller than before, free unused blocks */
	if (nr_blocks_old > inode->i_blocks) {
		/* Free unused blocks from page cache */
		truncate_pagecache(inode, i_size_read(inode));

		/* delete unused blocks. Note that we already CoW'ed the index block
		 * in write_begin, so this should be safe */
		if (__truncate_index_block(sb, ci->index_block, inode->i_blocks - 1)) {
			pr_err("failed truncating '%s'. we just lost %llu blocks\n",
			       file->f_path.dentry->d_name.name,
			       nr_blocks_old - inode->i_blocks);
			goto end;
		}
	}

end:
	return ret;
}

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	if ((wronly || rdwr) && trunc && (i_size_read(inode) != 0)) {
		struct super_block *sb = inode->i_sb;
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		int ret;

		/* Check if we can modify the index block, clone it otherwise */
		ret = ouichefs_cow_block(sb, &ci->index_block, OUICHEFS_INDEX);
		if (unlikely(ret < 0))
			return ret;

		/* Remove all blocks from index blocks */
		ret = __truncate_index_block(sb, ci->index_block, 0);
		if (ret)
			return ret;

		/* Update inode metadata */
		i_size_write(inode, 0);
		inode->i_blocks = 1;
		inode->i_ctime = current_time(inode);
		inode->i_mtime = current_time(inode);
		mark_inode_dirty(inode);
	}

	return 0;
}

/*
 * Internal function that puts all data blocks from 'start' until the end.
 * Assumptions:
 * - the given index block is valid and is not referenced multiple times
 */
static int __truncate_index_block(struct super_block *sb,
	uint32_t index_block, uint32_t start)
{
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;

	/* Read index block from disk */
	bh_index = sb_bread(sb, index_block);
	if (unlikely(!bh_index))
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/* Iterate all referenced blocks and dereference them */
	for (uint32_t i = start; i < OUICHEFS_INDEX_BLOCK_LEN; i++) {
		if (index->blocks[i] == 0)
			break;

		ouichefs_put_block(sb, index->blocks[i], OUICHEFS_DATA);
		index->blocks[i] = 0;
	}

	mark_buffer_dirty(bh_index);
	brelse(bh_index);

	return 0;
}

/*
 * Internal function to reflink files. Assumptions:
 * - the inodes as well as file mappings are locked
 * - dst is writeable
 * - dst and src have the same superblock
 *
 * This function doesn't update the metadata of the dst inode.
 *
 * Returns the number of bytes that were reflinked or an negative
 * error code.
 */
static ssize_t __reflink_file(struct ouichefs_inode_info *src,
	struct ouichefs_inode_info *dst)
{
	struct super_block *sb = src->vfs_inode.i_sb;
	int ret = 0;

	pr_debug("Reflinking inos %lu and %lu\n",
		src->vfs_inode.i_ino, dst->vfs_inode.i_ino);

	/* Check if files are already reflinked */
	if (src->index_block == dst->index_block)
		goto done;

	/* Get source index block */
	ret = ouichefs_get_block(sb, src->index_block);
	if (unlikely(ret))
		return ret;

	/* Put destination index block and point it to source */
	ouichefs_put_block(sb, dst->index_block,
		S_ISDIR(dst->vfs_inode.i_mode) ? OUICHEFS_DIR : OUICHEFS_INDEX);
	dst->index_block = src->index_block;

done:
	return i_size_read(&src->vfs_inode);
}

/*
 * Internal function to reflink blocks between files. Assumptions:
 * - the offsets and len are block-aligned
 * - the inodes as well as file mappings are locked
 * - dst is writeable and has data (at least) right up to dst_off,
 *   if its more than this, data is overwritten
 * - if dst == src, their ranges do not overlap
 * - dst and src have the same superblock
 *
 * This function doesn't update the metadata of the dst inode if
 * its size increased due to adding blocks.
 *
 * Returns the number of bytes that were reflinked or an negative
 * error code.
 */
static ssize_t __reflink_file_range(
	struct ouichefs_inode_info *src, loff_t src_off,
	struct ouichefs_inode_info *dst, loff_t dst_off, size_t len)
{
	struct super_block *sb = src->vfs_inode.i_sb;
	struct buffer_head *s_bh = NULL, *d_bh = NULL;
	struct ouichefs_file_index_block *src_index, *dst_index;
	bool mark_bh_dirty = false;
	ssize_t ret = 0;
	uint16_t s_off_b, d_off_b, len_b;

	/* Compute blocks to reflink */
	len_b = len / OUICHEFS_BLOCK_SIZE;
	s_off_b = src_off / OUICHEFS_BLOCK_SIZE;
	d_off_b = dst_off / OUICHEFS_BLOCK_SIZE;
	WARN_ON(len % OUICHEFS_BLOCK_SIZE != 0);
	WARN_ON(src_off % OUICHEFS_BLOCK_SIZE != 0);
	WARN_ON(dst_off % OUICHEFS_BLOCK_SIZE != 0);

	pr_debug("Reflinking %u blocks, src=%lu (at %u), dst=%lu (at %u)\n",
		len_b, src->vfs_inode.i_ino, s_off_b, dst->vfs_inode.i_ino, d_off_b);

	/* Open source index block */
	s_bh = sb_bread(sb, src->index_block);
	if (unlikely(!s_bh))
		return -EIO;
	src_index = (struct ouichefs_file_index_block *)s_bh->b_data;

	/* Clone dst index block is necessary */
	ret = ouichefs_cow_block(sb, &dst->index_block, OUICHEFS_INDEX);
	if (unlikely(ret < 0)) {
		brelse(s_bh);
		return ret;
	}
	if (ret > 0)
		mark_inode_dirty(&dst->vfs_inode);

	/* Open destination index block */
	d_bh = sb_bread(sb, dst->index_block);
	if (unlikely(!d_bh)) {
		brelse(s_bh);
		return -EIO;
	}
	dst_index = (struct ouichefs_file_index_block *)d_bh->b_data;

	/* Actually reflink the blocks */
	for (int i = 0; i < len_b; i++) {
		/* Check if blocks are already reflinked */
		if (src_index->blocks[s_off_b + i] ==
		    dst_index->blocks[d_off_b + i]) {
			ret += OUICHEFS_BLOCK_SIZE;
			continue;
		}

		/* Failed to access src data block, abort early */
		if (unlikely(
			ouichefs_get_block(sb, src_index->blocks[s_off_b + i]) < 0
		))
			goto early_out;

		/* Check if we overwrite a block and free it */
		if (dst_index->blocks[d_off_b + i])
			ouichefs_put_block(sb, dst_index->blocks[d_off_b + i],
				   OUICHEFS_DATA);
		dst_index->blocks[d_off_b + i] = src_index->blocks[s_off_b + i];
		mark_bh_dirty = true;

		ret += OUICHEFS_BLOCK_SIZE;
	}

	pr_debug("Reflinked %lu blocks (src=%lu, dst=%lu)\n",
		ret / OUICHEFS_BLOCK_SIZE, src->vfs_inode.i_ino, dst->vfs_inode.i_ino);

	/* Free index blocks */
early_out:
	if (mark_bh_dirty)
		mark_buffer_dirty(d_bh);
	brelse(d_bh);
	brelse(s_bh);

	return ret;
}

/*
 * Deduplicates or clones 'len' bytes between files. If len is zero, process
 * the whole source file. If REMAP_FILE_DEDUP, len mustn't be zero and the
 * given data regions must have equal content. If REMAP_FILE_CAN_SHORTEN and if
 * it makes sense, less than 'len' bytes may be processed to satisfy alignment.
 */
static loff_t ouichefs_remap_file_range(struct file *src_file, loff_t src_off,
	struct file *dst_file, loff_t dst_off, loff_t len, unsigned int flags)
{
	struct inode *src_ino = src_file->f_inode;
	struct inode *dst_ino = dst_file->f_inode;
	loff_t ret = 0;

	/* Filter for unknown flags; Abort if any are found */
	if (flags & ~(REMAP_FILE_DEDUP | REMAP_FILE_ADVISORY))
		return -EINVAL;

	pr_debug("Remapping %lld bytes from ino=%lu (off=%lld, size=%lld) to ino=%lu (off=%lld, size=%lld)\n",
		len, src_ino->i_ino, src_off, i_size_read(src_ino),
		dst_ino->i_ino, dst_off, i_size_read(dst_ino));

	/* Invalidate page cache of destination file */
	ret = invalidate_inode_pages2_range(dst_ino->i_mapping,
		dst_off >> PAGE_SHIFT,
		(dst_off + (len == 0 ? i_size_read(src_ino) : len)) >> PAGE_SHIFT);
	if (ret < 0) {
		pr_warn("Failed to invalidate inode pages: (%lld)\n", ret);
		ret = 0;
	}

	/* Lock inodes and page cache to prevent all data modification. See
	 * https://docs.kernel.org/filesystems/locking.html#file-operations */
	lock_two_nondirectories(src_ino, dst_ino);
	filemap_invalidate_lock_two(src_file->f_mapping, dst_file->f_mapping);

	/* This function checks offsets for validity, syncs file content,
	 * checks if blocks are equal and block-aligns 'len'
	 * if necessary. Also sets len to src file size if len is 0. */
	ret = generic_remap_file_range_prep(src_file, src_off,
		dst_file, dst_off, &len, flags);
	pr_debug("Update len=%lld", len);
	if (ret < 0 || len == 0)
		goto out_done;

	/* Can the whole file be reflinked? */
	if (src_off == 0 && dst_off == 0 &&
		len == i_size_read(src_ino) && len > i_size_read(dst_ino)) {
		ret = __reflink_file(OUICHEFS_INODE(src_ino), OUICHEFS_INODE(dst_ino));
		goto out_done;
	}

	/* Reflink requested blocks */
	ret = __reflink_file_range(OUICHEFS_INODE(src_ino), src_off,
		OUICHEFS_INODE(dst_ino), dst_off, len);

out_done:
	/* Update dest inode metadata if operation succeeded */
	if (ret > 0) {
		if (dst_off + ret > i_size_read(dst_ino)) {
			pr_debug("Update i_size %lld -> %llu\n", i_size_read(dst_ino), dst_off + ret);
			i_size_write(dst_ino, dst_off + ret);
			dst_ino->i_blocks = 1 + (i_size_read(dst_ino) / OUICHEFS_BLOCK_SIZE);
			if ((i_size_read(dst_ino) % OUICHEFS_BLOCK_SIZE) != 0)
				dst_ino->i_blocks++;
		}

		file_update_time(dst_file);
		mark_inode_dirty(dst_ino);
	}

	/* Unlock inodes and page cache */
	filemap_invalidate_unlock_two(src_file->f_mapping, dst_file->f_mapping);
	unlock_two_nondirectories(src_ino, dst_ino);

	return ret;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.remap_file_range = ouichefs_remap_file_range
};

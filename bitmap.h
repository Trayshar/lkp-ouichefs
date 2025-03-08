/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#ifndef _OUICHEFS_BITMAP_H
#define _OUICHEFS_BITMAP_H

#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/bitmap.h>

#include "ouichefs.h"

/*
 * Return the first free bit (set to 1) in a given in-memory bitmap spanning
 * over multiple blocks and clear it.
 * Return 0 if no free bit found (we assume that the first bit is never free
 * because of the superblock and the root inode, thus allowing us to use 0 as an
 * error value).
 */
static __always_inline uint32_t get_first_free_bit(unsigned long *freemap,
						   unsigned long size,
						   uint32_t *sb_counter,
						   spinlock_t *lock)
{
	uint32_t ino;

again:
	ino = find_first_bit(freemap, size);
	if (ino == size)
		return 0;

	spin_lock(lock);
	if (unlikely(!test_bit(ino, freemap))) {
		/* Someone else already got that bit, get a new one */
		spin_unlock(lock);
		goto again;
	}
	__clear_bit(ino, freemap);
	(*sb_counter)--;
	spin_unlock(lock);

	return ino;
}

/*
 * Return an unused inode number and mark it used.
 * Return 0 if no free inode was found.
 */
static inline uint32_t get_free_inode(struct ouichefs_sb_info *sbi)
{
	return get_first_free_bit(sbi->ifree_bitmap, sbi->nr_inodes,
				  &sbi->nr_free_inodes, &sbi->ifree_lock);
}

/*
 * Return an unused block number and mark it used.
 * Return 0 if no free block was found.
 */
static inline uint32_t get_free_block(struct ouichefs_sb_info *sbi)
{
	return get_first_free_bit(sbi->bfree_bitmap, sbi->nr_blocks,
				  &sbi->nr_free_blocks, &sbi->bfree_lock);
}

/*
 * Return an unused inode data entry number and mark it used.
 * Return 0 if no free entry was found.
 */
static inline uint32_t get_free_id_entry(struct ouichefs_sb_info *sbi)
{
	return get_first_free_bit(sbi->idfree_bitmap,
				  sbi->nr_inode_data_entries,
				  &sbi->nr_free_inode_data_entries,
				  &sbi->idfree_lock);
}

/*
 * Mark the i-th bit in freemap as free (i.e. 1)
 */
static __always_inline int put_free_bit(unsigned long *freemap,
					unsigned long size,
					uint32_t i, uint32_t *sb_counter,
					spinlock_t *lock)
{
	/* i is greater than freemap size */
	if (unlikely(i > size))
		return -1;

	spin_lock(lock);
	__set_bit(i, freemap);
	(*sb_counter)++;
	spin_unlock(lock);
	return 0;
}

/*
 * Mark an inode as unused.
 */
static inline void put_inode(struct ouichefs_sb_info *sbi, uint32_t ino)
{
	if (put_free_bit(sbi->ifree_bitmap, sbi->nr_inodes, ino,
			 &sbi->nr_free_inodes, &sbi->ifree_lock)) {
		return;
	}
	pr_debug("%s:%d: freed inode %u\n", __func__, __LINE__, ino);
}

/*
 * Mark a block as unused.
 */
static inline void put_block(struct ouichefs_sb_info *sbi, uint32_t bno)
{
	if (put_free_bit(sbi->bfree_bitmap, sbi->nr_blocks, bno,
			 &sbi->nr_free_blocks, &sbi->bfree_lock)) {
		return;
	}
	pr_debug("%s:%d: freed block %u\n", __func__, __LINE__, bno);
}

/*
 * Mark an inode data entry as unused.
 */
static inline void put_inode_data_entry(struct ouichefs_sb_info *sbi, uint32_t idx)
{
	if (put_free_bit(sbi->idfree_bitmap, sbi->nr_inode_data_entries,
			 idx, &sbi->nr_free_inode_data_entries,
			 &sbi->idfree_lock)) {
		return;
	}
	pr_debug("%s:%d: freed inode data entry %u\n", __func__, __LINE__, idx);
}

#endif /* _OUICHEFS_BITMAP_H */

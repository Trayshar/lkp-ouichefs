// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include "linux/buffer_head.h"
#include "linux/compiler.h"
#include "linux/pagemap.h"
#include "linux/printk.h"
#include "linux/stat.h"
#include <linux/errno.h>
#include <linux/fs.h>

#include "ouichefs.h"

static int copy_all_disk_inodes(struct super_block *sb,
				uint8_t from_index, uint8_t to_index)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct ouichefs_inode *disk_ino;
	uint32_t ino, last_ino_block = 0;
	bool dirty = false;

	/* Copy inodes on disk only, since we do not have a full list in memory */
	for_each_clear_bit(ino, sbi->ifree_bitmap, sbi->nr_inodes) {
		pr_debug("Copying ino %u\n", ino);
		/* Reuse buffer head between ino's if they are in the same block */
		if (OUICHEFS_GET_INODE_BLOCK(ino) != last_ino_block) {
			if (likely(bh)) {
				if (dirty) {
					mark_buffer_dirty(bh);
					sync_dirty_buffer(bh);
					dirty = false;
				}
				brelse(bh);
			}
			bh = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(ino));
			if (unlikely(!bh)) {
				pr_err("Failed to read inode %u while making a snapshot\n", ino);
				return -EIO;
			}
			last_ino_block = OUICHEFS_GET_INODE_BLOCK(ino);
		}
		disk_ino = (struct ouichefs_inode *) bh->b_data;
		disk_ino += OUICHEFS_GET_INODE_SHIFT(ino);

		// Inode exists in current snapshot; Copy it
		if (disk_ino->i_data[from_index] != 0) {
			ouichefs_link_inode_data(sb, ino, disk_ino,
						 from_index, to_index);
			dirty = true;
			pr_debug("Copied ino=%u\n", ino);
		}
	}
	if (likely(bh)) {
		if (dirty) {
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			dirty = false;
		}
		brelse(bh);
	}

	return 0;
}

int ouichefs_snapshot_create(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info *s_info = NULL;
	uint8_t new_snapshot_index;
	uint32_t new_snapshot_id = 0;
	int ret = 0;

	/* Find free index for new snapshot */
	for (ouichefs_snap_index_t j = 1; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
		if (sbi->snapshots[j].id == 0) {
			s_info = &sbi->snapshots[j];
			new_snapshot_index = j;
			break;
		}
	}

	/* Failed to find open spot for new snapshot; Abort! */
	if (s_info == NULL)
		return -ENOMEM;

	/* Find smallest free snapshot id */
	if (s_id == 0) {
		/* Find smallest free snapshot id */
		while (true) {
try_next_id:
			new_snapshot_id++;
			for (uint8_t j = 1; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
				if (sbi->snapshots[j].id == new_snapshot_id)
					goto try_next_id;
			}
			break;
		}
	} else {
		/* Check if given ID is free */
		for (uint8_t j = 1; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
			if (sbi->snapshots[j].id == s_id)
				return -EINVAL;
		}
		new_snapshot_id = s_id;
	}


	/* Sync all dirty data and prevent changes to this file system */
	ret = freeze_super(sb);
	if (ret) {
		pr_err("file system freeze failed\n");
		return ret;
	}

	/* Copy all inodes on disk */
	ret = copy_all_disk_inodes(sb, 0, new_snapshot_index);
	if (ret)
		goto cleanup;

	/* Update snapshot metadata in superblock */
	s_info->created = ktime_get_real_seconds();
	s_info->id = new_snapshot_id;
	pr_info("Created new snapshot %u\n", s_info->id);
	ret = 0;

cleanup:
	//unfreeze fs to unlock it
	if (thaw_super(sb))
		pr_err("File system unfreeze failed\n");

	return ret;
}

int ouichefs_snapshot_delete(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info *s_info = NULL;
	struct buffer_head *bh = NULL;
	struct ouichefs_inode *disk_ino;
	uint32_t ino, last_ino_block = 0;
	ouichefs_snap_index_t s_index;
	int ret = 0;
	bool dirty = false;

	// Cannot delete live snapshot
	if (s_id == 0)
		return -EINVAL;

	// Find snapshot to delete
	for (ouichefs_snap_index_t j = 1; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
		if (sbi->snapshots[j].id == s_id) {
			s_info = &sbi->snapshots[j];
			s_index = j;
			break;
		}
	}

	// Failed to find snapshot
	if (s_info == NULL)
		return -ENOENT;

	/* Sync all dirty data and prevent changes to this file system */
	ret = freeze_super(sb);
	if (ret) {
		pr_err("file system freeze failed\n");
		return ret;
	}

	// Clean up inodes on disk
	for_each_clear_bit(ino, sbi->ifree_bitmap, sbi->nr_inodes) {
		pr_debug("Iterating ino %u\n", ino);
		// Reuse buffer head between ino's if they are in the same block
		if (OUICHEFS_GET_INODE_BLOCK(ino) != last_ino_block) {
			if (likely(bh)) {
				if (dirty) {
					mark_buffer_dirty(bh);
					sync_dirty_buffer(bh);
					dirty = false;
				}
				brelse(bh);
			}
			bh = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(ino));
			if (unlikely(!bh)) {
				ret = -EIO;
				pr_err("Failed to read inode %u while deleting a snapshot\n", ino);
				goto cleanup;
			}
			last_ino_block = OUICHEFS_GET_INODE_BLOCK(ino);
		}
		disk_ino = (struct ouichefs_inode *) bh->b_data;
		disk_ino += OUICHEFS_GET_INODE_SHIFT(ino);

		// Inode exists in requested snapshot
		if (disk_ino->i_data[s_index] != 0) {
			ouichefs_put_inode_data(sb, ino, disk_ino, s_index);
			dirty = true;
		}
	}
	if (likely(bh)) {
		if (dirty) {
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			dirty = false;
		}
		brelse(bh);
	}

	/* Free the slot in the superblock */
	s_info->created = 0;
	s_info->id = 0;

	ret = 0;
cleanup:
	//unfreeze fs to unlock it
	if (thaw_super(sb))
		pr_err("File system unfreeze failed\n");

	return ret;
}

int ouichefs_snapshot_list(struct super_block *sb, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info s_info;
	struct tm *tm;
	ssize_t pos = 0;

	tm = kmalloc(sizeof(struct tm), GFP_KERNEL);

	/* Skip 0th entry as that is our private "live" snapshot */
	for (int i = 1; i < OUICHEFS_MAX_SNAPSHOTS; i++) {
		s_info = sbi->snapshots[i];
		if (s_info.id == 0) {
			continue;
		} else {
			time64_to_tm(s_info.created, 0, tm);
			pos += scnprintf(buf + pos, PAGE_SIZE - pos,
				"%u: %02d.%02d.%02ld %02d:%02d:%02d\n",
				s_info.id,
				tm->tm_mday, tm->tm_mon + 1, (tm->tm_year + 1900) % 100,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
		}
	}

	kfree(tm);

	return pos;
}

int ouichefs_snapshot_restore(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info *s_info = NULL;
	struct inode *i;
	ouichefs_snap_index_t s_index;
	int ret;

	// Cannot restore live snapshot
	if (s_id == 0)
		return -EINVAL;

	// Find snapshot
	for (uint8_t j = 1; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
		if (sbi->snapshots[j].id == s_id) {
			s_info = &sbi->snapshots[j];
			s_index = j;
			break;
		}
	}

	// Failed to find snapshot
	if (s_info == NULL)
		return -ENOENT;

	/* Sync all dirty data and prevent changes to this file system */
	ret = freeze_super(sb);
	if (ret) {
		pr_err("file system freeze failed\n");
		return ret;
	}

	/*
	 * Delete all dentries. This is overkill, but we do not have a
	 * topological view of the FS here (only a list of inodes), hence
	 * we can't selectively delete/update the dentries without doing a
	 * full tree traversal. We could technically use some flag in the inode
	 * to indicate if it's been deprecated and use d_revalidate to check it
	 * but thats a story for another time
	 */
	shrink_dcache_sb(sb);

	/*
	 * Copy all data on disk, as we might not have all inodes loaded currently.
	 */
	copy_all_disk_inodes(sb, s_index, 0);

	/*
	 * Delete all unused inodes (i_count=0) and update all others in memory.
	 * Usually, this is only the root inode.
	 */
	evict_inodes(sb);
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(i, &sb->s_inodes, i_sb_list) {
		pr_debug("Updating ino %lu, i_count=%u\n",
			 i->i_ino, atomic_read(&i->i_count));

		/* Load correct inode metadata and invalidate page cache */
		invalidate_inode_pages2(i->i_mapping);
		ret = ouichefs_ifill(i, false);
		if (ret) {
			/* Inode doesn't exist in this snapshot (or IO error) */
			if (S_ISDIR(i->i_mode)) {
				pr_debug("Mark ino %lu as dead\n", i->i_ino);
				i->i_flags |= S_DEAD;
			} else {
				/*
				 * Since this inode is still referenced, we
				 * can't just delete it. By invalidating all
				 * it's pages, no writes can succeed, and by
				 * setting I_DONTCACHE it will be evicted
				 * when it is eventually released. This
				 * should suffice.
				 */
				i->i_state |= I_DONTCACHE;
				pr_debug("Mark ino %lu as I_DONTCACHE\n",
					 i->i_ino);
			}
		} else if (i->i_flags & S_DEAD) {
			/* Inode does exist, but is marked as dead */
			pr_debug("Revive dead ino %lu\n", i->i_ino);
			i->i_flags &= ~S_DEAD;
		}
	}
	spin_unlock(&sb->s_inode_list_lock);

	//unfreeze fs to unlock it
	if (thaw_super(sb))
		pr_err("File system unfreeze failed\n");

	return 0;
}

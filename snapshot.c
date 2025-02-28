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

static int __snapshot_create(struct super_block *sb, bool is_restore)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info *s_info = NULL;
	struct buffer_head *bh = NULL;
	struct inode *i;
	struct ouichefs_inode *disk_ino;
	uint32_t ino, last_ino_block = 0;
	ouichefs_snap_index_t new_snapshot_index;
	int ret = 0;

	// Find free index for new snapshot
	for (ouichefs_snap_index_t j = 0; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
		if (sbi->snapshots[j].id == 0) {
			s_info = &sbi->snapshots[j];
			new_snapshot_index = j;
			break;
		}
	}

	// Failed to find open spot for new snapshot; Abort!
	if (s_info == NULL)
		return -ENOMEM;

	//freeze fs to lock it
	if (!is_restore) {
		ret = freeze_super(sb);
		if (ret) {
			pr_err("file system freeze failed\n");
			return ret;
		}
	}

	// Fill snapshot info
	s_info->created = ktime_get();
	s_info->id = sbi->next_snapshot_id;

	// Copy inodes on disk only, since we do not have a full list in memory
	for_each_clear_bit(ino, sbi->ifree_bitmap, sbi->nr_inodes) {
		pr_debug("Copying ino %u\n", ino);
		// Reuse buffer head between ino's if they are in the same block
		if (OUICHEFS_GET_INODE_BLOCK(ino) != last_ino_block) {
			if (likely(bh)) {
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);
			}
			bh = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(ino));
			if (unlikely(!bh)) {
				ret = -EIO;
				pr_err("Failed to read inode %u while making a snapshot\n", ino);
				// We may have successfully copied some inodes already, but
				// they will get overwritten the next time this snapshot
				// index is used, so it's not that bad; Main issue are the
				// data blocks that got their reference count incremented.
				// This means we will likely lose some storage capacity
				goto cleanup;
			}
			last_ino_block = OUICHEFS_GET_INODE_BLOCK(ino);
		}
		disk_ino = (struct ouichefs_inode *) bh->b_data;
		disk_ino += OUICHEFS_GET_INODE_SHIFT(ino);

		// Inode exists in current snapshot; Copy it
		if (disk_ino->i_data[sbi->current_snapshot_index].index_block != 0) {
			memcpy(
				&disk_ino->i_data[new_snapshot_index],
				&disk_ino->i_data[sbi->current_snapshot_index],
				sizeof(struct ouichefs_inode_data));
			ouichefs_get_block(sb,
				disk_ino->i_data[new_snapshot_index].index_block);
			pr_debug("Copied ino=%u\n", ino);
		}
	}
	if (likely(bh)) {
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	// Update all loaded inodes
	if (is_restore) {
		list_for_each_entry(i, &sb->s_inodes, i_sb_list) {
			if(OUICHEFS_INODE(i)->snapshot_id != OUICHEFS_GET_SNAP_ID(sbi))
				pr_debug("Found inode with s_id=%d, current is %d\n",
					OUICHEFS_INODE(i)->snapshot_id, OUICHEFS_GET_SNAP_ID(sbi));

			/* Load correct inode metadata and invalidate page cache */
			invalidate_inode_pages2(i->i_mapping);
			ret = ouichefs_ifill(i, false);
			if (ret) {
				/* Inode doesn't exist in this snapshot (or IO error) */
				pr_err("Not implemented: Invalidate ino %lu (c=%u)\n", i->i_ino, atomic_read(&i->i_count));
			}
		}
	} else {
		list_for_each_entry(i, &sb->s_inodes, i_sb_list) {
			if(OUICHEFS_INODE(i)->snapshot_id == OUICHEFS_GET_SNAP_ID(sbi)) {
				OUICHEFS_INODE(i)->snapshot_id = s_info->id;
			} else {
				pr_debug("Found inode with s_id=%d, current is %d\n",
					OUICHEFS_INODE(i)->snapshot_id, OUICHEFS_GET_SNAP_ID(sbi));
			}
		}
	}

	//Update snapshot metadata in superblock, making the new snapshot active
	pr_info("Mounted new snapshot %u, based on %u\n", s_info->id,
		OUICHEFS_GET_SNAP_ID(sbi));
	sbi->next_snapshot_id++;
	sbi->current_snapshot_index = new_snapshot_index;

	//unfreeze fs to unlock it
	if (!is_restore) {
		ret = thaw_super(sb);
		if (ret)
			pr_err("File system unfreeze failed\n");
	}

	return 0;

cleanup:
	s_info->created = 0;
	s_info->id = 0;

	//unfreeze fs to unlock it
	if (!is_restore) {
		ret = thaw_super(sb);
		if (ret)
			pr_err("File system unfreeze failed\n");
	}

	return ret;
}

int ouichefs_snapshot_create(struct super_block *sb)
{
	return __snapshot_create(sb, false);
}

int ouichefs_snapshot_delete(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info *s_info = NULL;
	struct buffer_head *bh = NULL;
	struct ouichefs_inode *disk_ino;
        struct ouichefs_inode_data *disk_ino_data;
	uint32_t ino, last_ino_block = 0;
	ouichefs_snap_index_t s_index;
	int ret = 0;

	// Cannot delete current snapshot
	if (s_id == OUICHEFS_GET_SNAP_ID(sbi))
		return -EINVAL;

	// Find snapshot to delete
	for (ouichefs_snap_index_t j = 0; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
		if (sbi->snapshots[j].id == s_id) {
			s_info = &sbi->snapshots[j];
			s_index = j;
			break;
		}
	}

	// Failed to find snapshot
	if (s_info == NULL)
		return -ENOENT;

	//freeze fs to lock it
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
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);
			}
			bh = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(ino));
			if (unlikely(!bh)) {
				ret = -EIO;
				pr_err("Failed to read inode %u while deleting a snapshot\n", ino);
				// We may have successfully copied some inodes already, but
				// they will get overwritten the next time this snapshot
				// index is used, so it's not that bad; Main issue are the
				// data blocks that got their reference count incremented.
				// This means we will likely lose some storage capacity

				//unfreeze fs to unlock it
				if (thaw_super(sb))
					pr_err("File system unfreeze failed\n");

				return ret;
			}
			last_ino_block = OUICHEFS_GET_INODE_BLOCK(ino);
		}
		disk_ino = (struct ouichefs_inode *) bh->b_data;
		disk_ino += OUICHEFS_GET_INODE_SHIFT(ino);
		disk_ino_data = &disk_ino->i_data[s_index];

		// Inode exists in requested snapshot
		if (disk_ino_data->index_block != 0) {
			ouichefs_put_block(sb,
				disk_ino_data->index_block,
				S_ISDIR(disk_ino_data->i_mode) ? OUICHEFS_DIR : OUICHEFS_INDEX);
			memset(disk_ino_data, 0,
				sizeof(struct ouichefs_inode_data));
			ouichefs_try_reclaim_disk_inode(disk_ino, sbi, ino);
		}
	}
	if (likely(bh)) {
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	//unfreeze fs to unlock it
	ret = thaw_super(sb);
	if (ret)
		pr_err("File system unfreeze failed\n");

	return 0;
}

int ouichefs_snapshot_list(struct super_block *sb)
{
	return -ENOTSUPP;
}

int ouichefs_snapshot_restore(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info *s_info = NULL;
	ouichefs_snap_index_t s_index;
	int ret = 0;

	// Cannot restore current snapshot
	if (s_id == OUICHEFS_GET_SNAP_ID(sbi))
		return -EINVAL;

	// Find snapshot
	for (ouichefs_snap_index_t j = 0; j < OUICHEFS_MAX_SNAPSHOTS; j++) {
		if (sbi->snapshots[j].id == s_id) {
			s_info = &sbi->snapshots[j];
			s_index = j;
			break;
		}
	}

	// Failed to find snapshot
	if (s_info == NULL)
		return -ENOENT;

	//freeze fs to lock it
	ret = freeze_super(sb);
	if (ret) {
		pr_err("file system freeze failed\n");
		return ret;
	}

	// Snapshots are read-only. Hence we cannot simply "mount" this snapshot.
	// We have to copy it first to get a writeable copy. Hence, we mount the
	// snapshot temporaily and then create a snapshot of it before unlocking.
	sbi->current_snapshot_index = s_index;
	ret = __snapshot_create(sb, true);
	if (ret)
		return ret;

	//unfreeze fs to unlock it
	ret = thaw_super(sb);
	if (ret)
		pr_err("File system unfreeze failed\n");

	return 0;
}
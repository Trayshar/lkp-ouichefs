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
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/statfs.h>

#include "ouichefs.h"

static struct kmem_cache *ouichefs_inode_cache;

int ouichefs_init_inode_cache(void)
{
	ouichefs_inode_cache = kmem_cache_create(
		"ouichefs_cache", sizeof(struct ouichefs_inode_info), 0, 0,
		NULL);
	if (!ouichefs_inode_cache)
		return -ENOMEM;
	return 0;
}

void ouichefs_destroy_inode_cache(void)
{
	kmem_cache_destroy(ouichefs_inode_cache);
}

static struct inode *ouichefs_alloc_inode(struct super_block *sb)
{
	struct ouichefs_inode_info *ci;

	/* ci = kzalloc(sizeof(struct ouichefs_inode_info), GFP_KERNEL); */
	ci = kmem_cache_alloc(ouichefs_inode_cache, GFP_KERNEL);
	if (!ci)
		return NULL;
	inode_init_once(&ci->vfs_inode);
	return &ci->vfs_inode;
}

static void ouichefs_destroy_inode(struct inode *inode)
{
	struct ouichefs_inode_info *ci;

	ci = OUICHEFS_INODE(inode);
	kmem_cache_free(ouichefs_inode_cache, ci);
}

/*
 * Writes an inode to disk, dropping associated data if it is deleted.
 */
static int ouichefs_write_inode(struct inode *inode,
				struct writeback_control *wbc)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	struct ouichefs_inode *disk_inode;
	struct ouichefs_inode_data *disk_idata;
	uint32_t ino = inode->i_ino;

	if (ino >= sbi->nr_inodes)
		return 0;

	/*
	 * This inode was deleted in ouichefs_unlink(), and its associated
	 * inode data was already released. We cannot write to it anymore,
	 * which means we're done here. I call this an absolute win!
	 */
	if (ci->index_block == 0) {
		/* This is here for debugging, gate behind a flag maybe? */
		bh = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(ino));
		if (unlikely(!bh))
			return -EIO;
		disk_inode = (struct ouichefs_inode *)bh->b_data;
		disk_inode += OUICHEFS_GET_INODE_SHIFT(ino);

		if (unlikely(disk_inode->i_data[0])) {
			pr_err("Dead inode %u has idx %u mapped!",
			       ino, disk_inode->i_data[0]);
		} else {
			pr_debug("Skip writing dead inode %u (idx %u)\n",
				 ino, disk_inode->i_data[0]);
		}
		brelse(bh);
		return 0;
	}

	/* Get inode data from disk */
	disk_idata = ouichefs_get_inode_data(sb, &bh, ino, false, true);
	if (IS_ERR(disk_idata))
		return PTR_ERR(disk_idata);

	/* update the mode using what the generic inode has */
	disk_idata->i_mode = inode->i_mode;
	disk_idata->i_uid = i_uid_read(inode);
	disk_idata->i_gid = i_gid_read(inode);
	disk_idata->i_size = i_size_read(inode);
	disk_idata->i_ctime = inode->i_ctime.tv_sec;
	disk_idata->i_nctime = inode->i_ctime.tv_nsec;
	disk_idata->i_atime = inode->i_atime.tv_sec;
	disk_idata->i_natime = inode->i_atime.tv_nsec;
	disk_idata->i_mtime = inode->i_mtime.tv_sec;
	disk_idata->i_nmtime = inode->i_mtime.tv_nsec;
	disk_idata->i_blocks = inode->i_blocks;
	disk_idata->i_nlink = inode->i_nlink;
	disk_idata->index_block = ci->index_block;

	pr_debug("Wrote inode %u with index_block %u\n", ino, ci->index_block);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_sb_info(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_sb_info *disk_sb;
	struct buffer_head *bh;

	/* Flush superblock */
	bh = sb_bread(sb, OUICHEFS_SB_BLOCK_NR);
	if (!bh)
		return -EIO;
	disk_sb = (struct ouichefs_sb_info *)bh->b_data;

	disk_sb->nr_blocks = sbi->nr_blocks;
	disk_sb->nr_inodes = sbi->nr_inodes;
	disk_sb->nr_inode_data_entries = sbi->nr_inode_data_entries;
	disk_sb->nr_istore_blocks = sbi->nr_istore_blocks;
	disk_sb->nr_ifree_blocks = sbi->nr_ifree_blocks;
	disk_sb->nr_bfree_blocks = sbi->nr_bfree_blocks;
	disk_sb->nr_free_inodes = sbi->nr_free_inodes;
	disk_sb->nr_free_blocks = sbi->nr_free_blocks;
	disk_sb->nr_free_inode_data_entries = sbi->nr_free_inode_data_entries;
	disk_sb->nr_idfree_blocks = sbi->nr_idfree_blocks;
	disk_sb->nr_ididx_blocks = sbi->nr_ididx_blocks;
	disk_sb->nr_meta_blocks = sbi->nr_meta_blocks;
	memcpy(disk_sb->snapshots, sbi->snapshots,
		sizeof(disk_sb->snapshots));

	mark_buffer_dirty(bh);
	if (wait)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_bitmap(struct super_block *sb, unsigned long *bitmap,
		       uint32_t nr_blocks, uint32_t start, bool wait)
{
	struct buffer_head *bh;
	int i, idx;

	/* Flush free blocks / inodes / ididx bitmask */
	for (i = 0; i < nr_blocks; i++) {
		idx = start + i;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		memcpy(bh->b_data,
		       (void *)bitmap + i * OUICHEFS_BLOCK_SIZE,
		       OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static int load_bitmap(struct super_block *sb, unsigned long **bitmap,
		       uint32_t nr_blocks, uint32_t start)
{
	struct buffer_head *bh;
	*bitmap = kzalloc(nr_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);

	if (!(*bitmap))
		return -ENOMEM;
	for (int i = 0; i < nr_blocks; i++) {
		int idx = start + i;

		bh = sb_bread(sb, idx);
		if (!bh) {
			kfree(*bitmap);
			return -EIO;
		}

		memcpy((void *)(*bitmap) + i * OUICHEFS_BLOCK_SIZE,
		       bh->b_data, OUICHEFS_BLOCK_SIZE);

		brelse(bh);
	}
	return 0;
}

static void ouichefs_put_super(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi) {
		kfree(sbi->ifree_bitmap);
		kfree(sbi->bfree_bitmap);
		kfree(sbi->idfree_bitmap);
		kfree(sbi);
	}
}

static int ouichefs_sync_fs(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	int ret = 0;

	ret = sync_sb_info(sb, wait);
	if (ret)
		return ret;
	ret = sync_bitmap(sb, sbi->ifree_bitmap, sbi->nr_ifree_blocks,
			  OUICHEFS_GET_IFREE_START(sbi), wait);
	if (ret)
		return ret;
	ret = sync_bitmap(sb, sbi->bfree_bitmap, sbi->nr_bfree_blocks,
			  OUICHEFS_GET_BFREE_START(sbi), wait);
	if (ret)
		return ret;
	ret = sync_bitmap(sb, sbi->idfree_bitmap, sbi->nr_idfree_blocks,
			  OUICHEFS_GET_IDFREE_START(sbi), wait);
	if (ret)
		return ret;

	return 0;
}

static int ouichefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
	struct super_block *sb = dentry->d_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	stat->f_type = OUICHEFS_MAGIC;
	stat->f_bsize = OUICHEFS_BLOCK_SIZE;
	stat->f_blocks = sbi->nr_blocks;
	stat->f_bfree = sbi->nr_free_blocks;
	stat->f_bavail = sbi->nr_free_blocks;
	stat->f_files = sbi->nr_inodes;
	stat->f_ffree = sbi->nr_free_inodes;
	stat->f_namelen = OUICHEFS_FILENAME_LEN;

	return 0;
}

static struct super_operations ouichefs_super_ops = {
	.put_super = ouichefs_put_super,
	.alloc_inode = ouichefs_alloc_inode,
	.destroy_inode = ouichefs_destroy_inode,
	.write_inode = ouichefs_write_inode,
	.sync_fs = ouichefs_sync_fs,
	.statfs = ouichefs_statfs,
};

/* Fill the struct superblock from partition superblock */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh = NULL;
	struct ouichefs_sb_info *csb = NULL;
	struct ouichefs_sb_info *sbi = NULL;
	struct inode *root_inode = NULL;
	int ret = 0;

	/* Init sb */
	sb->s_magic = OUICHEFS_MAGIC;
	sb_set_blocksize(sb, OUICHEFS_BLOCK_SIZE);
	sb->s_maxbytes = OUICHEFS_MAX_FILESIZE;
	sb->s_op = &ouichefs_super_ops;
	sb->s_time_gran = 1;

	/* Read sb from disk */
	bh = sb_bread(sb, OUICHEFS_SB_BLOCK_NR);
	if (!bh)
		return -EIO;
	csb = (struct ouichefs_sb_info *)bh->b_data;

	/* Check magic number */
	if (csb->magic != sb->s_magic) {
		pr_err("Wrong magic number\n");
		brelse(bh);
		return -EPERM;
	}

	/* Alloc sb_info */
	sbi = kzalloc(sizeof(struct ouichefs_sb_info), GFP_KERNEL);
	if (!sbi) {
		brelse(bh);
		return -ENOMEM;
	}
	sbi->nr_blocks = csb->nr_blocks;
	sbi->nr_inodes = csb->nr_inodes;
	sbi->nr_inode_data_entries = csb->nr_inode_data_entries;
	sbi->nr_istore_blocks = csb->nr_istore_blocks;
	sbi->nr_ifree_blocks = csb->nr_ifree_blocks;
	sbi->nr_bfree_blocks = csb->nr_bfree_blocks;
	sbi->nr_free_inodes = csb->nr_free_inodes;
	sbi->nr_free_blocks = csb->nr_free_blocks;
	sbi->nr_free_inode_data_entries = csb->nr_free_inode_data_entries;
	sbi->nr_idfree_blocks = csb->nr_idfree_blocks;
	sbi->nr_ididx_blocks = csb->nr_ididx_blocks;
	sbi->nr_meta_blocks = csb->nr_meta_blocks;
	memcpy(sbi->snapshots, csb->snapshots,
		sizeof(sbi->snapshots));
	sb->s_fs_info = sbi;

	brelse(bh);

	/* Alloc and copy ifree_bitmap */
	spin_lock_init(&sbi->ifree_lock);
	if (load_bitmap(sb, &sbi->ifree_bitmap, sbi->nr_ifree_blocks,
			OUICHEFS_GET_IFREE_START(sbi)))
		goto free_sbi;

	/* Alloc and copy bfree_bitmap */
	spin_lock_init(&sbi->bfree_lock);
	if (load_bitmap(sb, &sbi->bfree_bitmap, sbi->nr_bfree_blocks,
			OUICHEFS_GET_BFREE_START(sbi)))
		goto free_ifree;

	/* Alloc and copy idfree_bitmap */
	spin_lock_init(&sbi->idfree_lock);
	if (load_bitmap(sb, &sbi->idfree_bitmap, sbi->nr_idfree_blocks,
			OUICHEFS_GET_IDFREE_START(sbi)))
		goto free_bfree;

	/* Create root inode */
	root_inode = ouichefs_iget(sb, 1, false);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		pr_warn("Failed to load root inode: %d\n", ret);
		goto free_idfree;
	}
	if (!S_ISDIR(root_inode->i_mode)) {
		ret = -ENOTDIR;
		pr_debug("Failed to load root inode: not an directory, mode is %u\n",
			 root_inode->i_mode);
		goto iput;
	}
	inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto iput;
	}

	pr_debug("Loaded superblock:\n"
		 "\tnr_blocks=%u\n"
		 "\tnr_inodes=%u (istore=%u blocks)\n"
		 "\tnr_inode_data_entries=%u (ididx=%u blocks)\n"
		 "\tnr_ifree_blocks=%u\n"
		 "\tnr_bfree_blocks=%u\n"
		 "\tnr_idfree_blocks=%u\n"
		 "\tnr_meta_blocks=%u\n"
		 "\tnr_free_inodes=%u\n"
		 "\tnr_free_blocks=%u\n"
		 "\tnr_free_inode_data_entries=%u\n"
		 "\tINODE_START=%u\n"
		 "\tIFREE_START=%u\n"
		 "\tBFREE_START=%u\n"
		 "\tMETA_START=%u\n"
		 "\tDATA_START=%u\n",
		 sbi->nr_blocks, sbi->nr_inodes, sbi->nr_istore_blocks,
		 sbi->nr_inode_data_entries, sbi->nr_ididx_blocks,
		 sbi->nr_ifree_blocks, sbi->nr_bfree_blocks,
		 sbi->nr_idfree_blocks, sbi->nr_meta_blocks,
		 sbi->nr_free_inodes, sbi->nr_free_blocks,
		 sbi->nr_free_inode_data_entries,
		 OUICHEFS_GET_INODE_BLOCK(0), OUICHEFS_GET_IFREE_START(sbi),
		 OUICHEFS_GET_BFREE_START(sbi),
		 OUICHEFS_GET_META_BLOCK(OUICHEFS_GET_DATA_START(sbi) + 1, sbi),
		 OUICHEFS_GET_DATA_START(sbi)
	);

	return 0;

iput:
	iput(root_inode);
free_idfree:
	kfree(sbi->idfree_bitmap);
free_bfree:
	kfree(sbi->bfree_bitmap);
free_ifree:
	kfree(sbi->ifree_bitmap);
free_sbi:
	kfree(sbi);

	return ret;
}

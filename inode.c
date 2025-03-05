// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#include "linux/compiler.h"
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/printk.h>

#include "ouichefs.h"
#include "bitmap.h"

static const struct inode_operations ouichefs_inode_ops;

inline bool ouichefs_inode_needs_update(struct inode* inode)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(inode->i_sb);
	return OUICHEFS_INODE(inode)->snapshot_id != OUICHEFS_GET_SNAP_ID(sbi);
}

/*
 * Internal function that updates a given inode to the state it has on disk
 * in the given snapshot. Return 0 on success, -EINVAL if the inode is deleted
 * in that snapshot, and -EIO if the inode block cannot be read.
 */
int ouichefs_ifill(struct inode* inode, bool create)
{
	
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_inode_data *cinode = NULL;
	struct ouichefs_inode *disk_inode = NULL;
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	int ret = 0;

	pr_debug("Loading inode %lu from disk (snapshot %d)\n",
		inode->i_ino, OUICHEFS_GET_SNAP_ID(sbi));

	/* Read inode from disk and initialize */
	bh = sb_bread(sb, OUICHEFS_GET_INODE_BLOCK(inode->i_ino));
	if (unlikely(!bh))
		return -EIO;
	disk_inode = (struct ouichefs_inode *)bh->b_data;
	disk_inode += OUICHEFS_GET_INODE_SHIFT(inode->i_ino);

	/* Index the disk inode at the current snapshot */
	cinode = &disk_inode->i_data[sbi->current_snapshot_index];

	/* Check if this inode still exists in the current snapshot */
	if (cinode->index_block == 0 && !create) {
		ret = -EINVAL;
		goto failed;
	}

	inode->i_mode = le32_to_cpu(cinode->i_mode);
	i_uid_write(inode, le32_to_cpu(cinode->i_uid));
	i_gid_write(inode, le32_to_cpu(cinode->i_gid));
	i_size_write(inode, le32_to_cpu(cinode->i_size));
	inode->i_ctime.tv_sec = (time64_t)le32_to_cpu(cinode->i_ctime);
	inode->i_ctime.tv_nsec = (long)le64_to_cpu(cinode->i_nctime);
	inode->i_atime.tv_sec = (time64_t)le32_to_cpu(cinode->i_atime);
	inode->i_atime.tv_nsec = (long)le64_to_cpu(cinode->i_natime);
	inode->i_mtime.tv_sec = (time64_t)le32_to_cpu(cinode->i_mtime);
	inode->i_mtime.tv_nsec = (long)le64_to_cpu(cinode->i_nmtime);
	inode->i_blocks = le32_to_cpu(cinode->i_blocks);
	set_nlink(inode, le32_to_cpu(cinode->i_nlink));

	ci->index_block = le32_to_cpu(cinode->index_block);
	ci->snapshot_id = OUICHEFS_GET_SNAP_ID(sbi);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_fop = &ouichefs_dir_ops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_fop = &ouichefs_file_ops;
		inode->i_mapping->a_ops = &ouichefs_aops;
	}

failed:
	brelse(bh);
	return ret;
}

/*
 * Get inode ino from disk.
 */
struct inode *ouichefs_iget(struct super_block *sb, uint32_t ino, bool create)
{
	struct inode *inode = NULL;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t inode_block = OUICHEFS_GET_INODE_BLOCK(ino);
	uint32_t inode_shift = OUICHEFS_GET_INODE_SHIFT(ino);
	int ret;

	pr_debug("ino=%u, inode_block=%u, inode_shift=%u, snapindex=%u, create=%i\n",
		ino, inode_block, inode_shift, sbi->current_snapshot_index, create);

	/* Fail if ino is out of range */
	if (ino >= sbi->nr_inodes)
		return ERR_PTR(-EINVAL);

	/* Get a locked inode from Linux */
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	/* If inode is in cache, return it */
	if (!(inode->i_state & I_NEW)) {
		/* Update inode data if current snapshot changed */
		if (ouichefs_inode_needs_update(inode)) {
			pr_debug("Updating inode from snapshot %d\n",
				OUICHEFS_INODE(inode)->snapshot_id);
			ret = ouichefs_ifill(inode, false);
			if (ret)
				return ERR_PTR(ret);
		}
		return inode;
	}


	/* Loading new inode */
	inode->i_ino = ino;
	inode->i_sb = sb;
	inode->i_op = &ouichefs_inode_ops;
	ret = ouichefs_ifill(inode, create);
	if (!ret) {
		/* Unlock the inode to make it usable */
		unlock_new_inode(inode);
		return inode;
	}

	iget_failed(inode);
	return ERR_PTR(ret);
}

/*
 * Look for dentry in dir.
 * Fill dentry with NULL if not in dir, with the corresponding inode if found.
 * Returns NULL on success.
 */
static struct dentry *ouichefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct ouichefs_inode_info *ci_dir = OUICHEFS_INODE(dir);
	struct inode *inode = NULL;
	struct buffer_head *bh = NULL;
	struct ouichefs_dir_block *dblock = NULL;
	struct ouichefs_file *f = NULL;
	int i;

	pr_debug("dir=%lu (snap %d), dentry=%s\n",
		dir->i_ino, ci_dir->snapshot_id, dentry->d_name.name);

	/* Check filename length */
	if (dentry->d_name.len > OUICHEFS_FILENAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	/* Read the directory index block on disk */
	bh = sb_bread(sb, ci_dir->index_block);
	if (!bh)
		return ERR_PTR(-EIO);
	dblock = (struct ouichefs_dir_block *)bh->b_data;

	/* Search for the file in directory */
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		f = &dblock->files[i];
		if (!f->inode)
			break;
		if (!strncmp(f->filename, dentry->d_name.name,
			     OUICHEFS_FILENAME_LEN)) {
			inode = ouichefs_iget(sb, f->inode, false);
			if (IS_ERR(inode)) {
				inode = NULL;
				continue;
			}
			break;
		}
	}
	brelse(bh);

	/* Update directory access time */
	dir->i_atime = current_time(dir);
	mark_inode_dirty(dir);

	/* Fill the dentry with the inode */
	d_add(dentry, inode);

	return NULL;
}

/*
 * Create a new inode in dir.
 */
static struct inode *ouichefs_new_inode(struct inode *dir, mode_t mode)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct super_block *sb;
	struct ouichefs_sb_info *sbi;
	uint32_t ino, bno;
	int ret;

	/* Check mode before doing anything to avoid undoing everything */
	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		pr_err("File type not supported (only directory and regular files supported)\n");
		return ERR_PTR(-EINVAL);
	}

	/* Check if inodes are available */
	sb = dir->i_sb;
	sbi = OUICHEFS_SB(sb);
	if (sbi->nr_free_inodes == 0 || sbi->nr_free_blocks == 0)
		return ERR_PTR(-ENOSPC);

	/* Get a new free inode */
	ino = get_free_inode(sbi);
	if (!ino)
		return ERR_PTR(-ENOSPC);
	inode = ouichefs_iget(sb, ino, true);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto put_ino;
	}
	ci = OUICHEFS_INODE(inode);

	/* Get a free block for this new inode's index */
	ret = ouichefs_alloc_block(sb, &bno);
	if (ret < 0)
		goto put_inode;
	ci->index_block = bno;

	/* Initialize inode */
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_blocks = 1;
	if (S_ISDIR(mode)) {
		i_size_write(inode, OUICHEFS_BLOCK_SIZE);
		inode->i_fop = &ouichefs_dir_ops;
		set_nlink(inode, 2); /* . and .. */
	} else if (S_ISREG(mode)) {
		i_size_write(inode, 0);
		inode->i_fop = &ouichefs_file_ops;
		inode->i_mapping->a_ops = &ouichefs_aops;
		set_nlink(inode, 1);
	}

	inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
	pr_debug("Created inode %u (index block %u)\n", ino, bno);

	return inode;

put_inode:
	iput(inode);
put_ino:
	put_inode(sbi, ino);

	return ERR_PTR(ret);
}

/*
 * Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
static int ouichefs_create(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	uint32_t dir_index_block = OUICHEFS_INODE(dir)->index_block;
	struct ouichefs_dir_block *dblock;
	char *fblock;
	struct buffer_head *bh, *bh2;
	int ret = 0, i;

	/* Check filename length */
	if (strlen(dentry->d_name.name) > OUICHEFS_FILENAME_LEN)
		return -ENAMETOOLONG;

	/* Check that we can modify the directory block, clone it otherwise */
	ret = ouichefs_cow_block(sb, &dir_index_block, OUICHEFS_DIR);
	if (unlikely(ret < 0))
		return ret;

	/* Read parent directory index */
	bh = sb_bread(sb, dir_index_block);
	if (unlikely(!bh)) {
		ret = -EIO;
		goto end;
	}
	dblock = (struct ouichefs_dir_block *)bh->b_data;

	/* Check if parent directory is full */
	if (dblock->files[OUICHEFS_MAX_SUBFILES - 1].inode != 0) {
		ret = -EMLINK;
		goto end;
	}

	/* Get a new free inode */
	inode = ouichefs_new_inode(dir, mode);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto end;
	}

	/*
	 * Scrub index_block for new file/directory to avoid previous data
	 * messing with new file/directory.
	 */
	bh2 = sb_bread(sb, OUICHEFS_INODE(inode)->index_block);
	if (!bh2) {
		ret = -EIO;
		goto iput;
	}
	fblock = (char *)bh2->b_data;
	memset(fblock, 0, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh2);
	brelse(bh2);

	/* Find first free slot in parent index and register new inode */
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++)
		if (dblock->files[i].inode == 0)
			break;
	dblock->files[i].inode = inode->i_ino;
	strscpy(dblock->files[i].filename, dentry->d_name.name,
		OUICHEFS_FILENAME_LEN);
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Update stats and mark dir and new inode dirty */
	mark_inode_dirty(inode);
	dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(mode))
		inode_inc_link_count(dir);
	OUICHEFS_INODE(dir)->index_block = dir_index_block;
	mark_inode_dirty(dir);

	/* setup dentry */
	d_instantiate(dentry, inode);

	return 0;

iput:
        /* 
         * Since this inode was just created, we can skip the cleanup 
         * logic in ouichefs_put_block by pretending it's just data
         */
	ouichefs_put_block(sb, OUICHEFS_INODE(inode)->index_block, OUICHEFS_DATA);
	put_inode(OUICHEFS_SB(sb), inode->i_ino);
	iput(inode);
end:
	brelse(bh);
	if (dir_index_block != OUICHEFS_INODE(dir)->index_block)
		ouichefs_put_block(sb, dir_index_block, OUICHEFS_DIR);
	return ret;
}

/*
 * Remove a link for a file. If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int ouichefs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh = NULL;
	struct ouichefs_dir_block *dir_block = NULL;
	bool is_dir = S_ISDIR(inode->i_mode);
	uint32_t ino = inode->i_ino;
	uint32_t dir_index_block = OUICHEFS_INODE(dir)->index_block;
	uint32_t bno;
	int i, ret, f_id, nr_subs;

	f_id = -1;
	bno = OUICHEFS_INODE(inode)->index_block;

	/* Check that we can modify the directory block, clone it otherwise */
	ret = ouichefs_cow_block(sb, &dir_index_block, OUICHEFS_DIR);
	if (unlikely(ret < 0))
		return ret;

	/* Read parent directory index */
	bh = sb_bread(sb, dir_index_block);
	if (unlikely(!bh)) {
		ret = -EIO;
		goto failed;
	}
	dir_block = (struct ouichefs_dir_block *)bh->b_data;

	/* Search for inode in parent index and get number of subfiles */
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		if (dir_block->files[i].inode == ino)
			f_id = i;
		else if (dir_block->files[i].inode == 0)
			break;
	}
	nr_subs = i;

	/* Remove file from parent directory */
	if (f_id != OUICHEFS_MAX_SUBFILES - 1)
		memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
			(nr_subs - f_id - 1) * sizeof(struct ouichefs_file));
	memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct ouichefs_file));
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Update inode stats */
	dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (is_dir)
		inode_dec_link_count(dir);
	OUICHEFS_INODE(dir)->index_block = dir_index_block;
	mark_inode_dirty(dir);

	/* Cleanup inode and mark dirty */
	inode->i_blocks = 0;
	OUICHEFS_INODE(inode)->index_block = 0;
	i_size_write(inode, 0);
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	inode->i_mode = 0;
	inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec =
		0;
	inode->i_ctime.tv_nsec = inode->i_mtime.tv_nsec =
		inode->i_atime.tv_nsec = 0;
	inode_dec_link_count(inode);
	mark_inode_dirty(inode);

	/* Free inode and index block from bitmap, as well as clean
	 * all associated data */
	ouichefs_put_block(sb, bno, is_dir ? OUICHEFS_DIR : OUICHEFS_INDEX);
	pr_debug("Freed inode %u (index block %u)\n", ino, bno);

	return 0;

failed:
	if (dir_index_block != OUICHEFS_INODE(dir)->index_block)
		ouichefs_put_block(sb, dir_index_block, OUICHEFS_DIR);
	return ret;
}

static int ouichefs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			   struct dentry *old_dentry, struct inode *new_dir,
			   struct dentry *new_dentry, unsigned int flags)
{
	struct super_block *sb = old_dir->i_sb;
	struct ouichefs_inode_info *ci_old = OUICHEFS_INODE(old_dir);
	struct ouichefs_inode_info *ci_new = OUICHEFS_INODE(new_dir);
	struct inode *src = d_inode(old_dentry);
	struct buffer_head *bh_old = NULL, *bh_new = NULL;
	struct ouichefs_dir_block *dir_block = NULL;
	int i, f_id = -1, new_pos = -1, ret, nr_subs, f_pos = -1;
	uint32_t new_index_block = ci_new->index_block;

	/* fail with these unsupported flags */
	if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
		return -EINVAL;

	/* Check if filename is not too long */
	if (strlen(new_dentry->d_name.name) > OUICHEFS_FILENAME_LEN)
		return -ENAMETOOLONG;

	/* Check that we can modify the directory block, clone it otherwise */
	ret = ouichefs_cow_block(sb, &new_index_block, OUICHEFS_DIR);
	if (unlikely(ret < 0))
		return ret;

	/* Fail if new_dentry exists or if new_dir is full */
	bh_new = sb_bread(sb, new_index_block);
	if (!bh_new)
		return -EIO;
	dir_block = (struct ouichefs_dir_block *)bh_new->b_data;
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		/* if old_dir == new_dir, save the renamed file position */
		if (new_dir == old_dir) {
			if (strncmp(dir_block->files[i].filename,
				    old_dentry->d_name.name,
				    OUICHEFS_FILENAME_LEN) == 0)
				f_pos = i;
		}
		if (strncmp(dir_block->files[i].filename,
			    new_dentry->d_name.name,
			    OUICHEFS_FILENAME_LEN) == 0) {
			ret = -EEXIST;
			goto relse_new;
		}
		if (new_pos < 0 && dir_block->files[i].inode == 0)
			new_pos = i;
	}
	/* if old_dir == new_dir, just rename entry */
	if (old_dir == new_dir) {
		strscpy(dir_block->files[f_pos].filename,
			new_dentry->d_name.name, OUICHEFS_FILENAME_LEN);
		mark_buffer_dirty(bh_new);
		ret = 0;
		goto relse_new;
	}

	/* If new directory is empty, fail */
	if (new_pos < 0) {
		ret = -EMLINK;
		goto relse_new;
	}

	/* insert in new parent directory */
	dir_block->files[new_pos].inode = src->i_ino;
	strscpy(dir_block->files[new_pos].filename, new_dentry->d_name.name,
		OUICHEFS_FILENAME_LEN);
	mark_buffer_dirty(bh_new);
	brelse(bh_new);

	/* Update new parent inode metadata */
	new_dir->i_atime = new_dir->i_ctime = new_dir->i_mtime =
		current_time(new_dir);
	if (S_ISDIR(src->i_mode))
		inode_inc_link_count(new_dir);
	ci_new->index_block = new_index_block;
	mark_inode_dirty(new_dir);

	/* remove target from old parent directory */
	ret = ouichefs_cow_block(sb, &ci_old->index_block, OUICHEFS_DIR);
	if (unlikely(ret < 0))
		return ret;
	bh_old = sb_bread(sb, ci_old->index_block);
	if (!bh_old)
		return -EIO;
	dir_block = (struct ouichefs_dir_block *)bh_old->b_data;

	/* Search for inode in old directory and number of subfiles */
	for (i = 0; OUICHEFS_MAX_SUBFILES; i++) {
		if (dir_block->files[i].inode == src->i_ino)
			f_id = i;
		else if (dir_block->files[i].inode == 0)
			break;
	}
	nr_subs = i;

	/* Remove file from old parent directory */
	if (f_id != OUICHEFS_MAX_SUBFILES - 1)
		memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
			(nr_subs - f_id - 1) * sizeof(struct ouichefs_file));
	memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct ouichefs_file));
	mark_buffer_dirty(bh_old);
	brelse(bh_old);

	/* Update old parent inode metadata */
	old_dir->i_atime = old_dir->i_ctime = old_dir->i_mtime =
		current_time(old_dir);
	if (S_ISDIR(src->i_mode))
		inode_dec_link_count(old_dir);
	mark_inode_dirty(old_dir);

	return 0;

relse_new:
	brelse(bh_new);
	ouichefs_put_block(sb, new_index_block, OUICHEFS_DIR);
	return ret;
}

static int ouichefs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode)
{
	return ouichefs_create(NULL, dir, dentry, mode | S_IFDIR, 0);
}

static int ouichefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh;
	struct ouichefs_dir_block *dblock;

	/* If the directory is not empty, fail */
	if (inode->i_nlink > 2)
		return -ENOTEMPTY;
	bh = sb_bread(sb, OUICHEFS_INODE(inode)->index_block);
	if (!bh)
		return -EIO;
	dblock = (struct ouichefs_dir_block *)bh->b_data;
	if (dblock->files[0].inode != 0) {
		brelse(bh);
		return -ENOTEMPTY;
	}
	brelse(bh);

	/* Remove directory with unlink */
	return ouichefs_unlink(dir, dentry);
}

static const struct inode_operations ouichefs_inode_ops = {
	.lookup = ouichefs_lookup,
	.create = ouichefs_create,
	.unlink = ouichefs_unlink,
	.mkdir = ouichefs_mkdir,
	.rmdir = ouichefs_rmdir,
	.rename = ouichefs_rename,
};

// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/fs.h>
#include "ouichefs.h"


int ouichefs_snapshot_create(struct super_block *sb)
{
	int ret = 0;

	//allocate new snapshot
	struct ouichefs_snapshot_info *sn = kmalloc(sizeof(struct ouichefs_snapshot_info), GFP_KERNEL);

	if (!sn)
		return -ENOMEM;

    //freeze fs to lock it
	ret = freeze_super(sb);

	if (ret) {
		pr_err("file system freeze failed\n");
		goto cleanup;
	}


    //copy inodes

    //change root inode of fs to be root inode of new snapshot

    //unfreeze fs to unlock it

	ret = thaw_super(sb);

	if (ret) {
		pr_err("File system unfreeze failed\n");
		goto cleanup;
	}

	return 0;

cleanup:
	kfree(sn);
	return ret;
}

int ouichefs_snapshot_delete(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	return -ENOTSUPP;
}

int ouichefs_snapshot_list(struct super_block *sb)
{
	return -ENOTSUPP;
}

int ouichefs_snapshot_restore(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	return -ENOTSUPP;
}
